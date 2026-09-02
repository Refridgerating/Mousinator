"""Local browser server and CLI for the Sentry Raspberry Pi camera."""

from __future__ import annotations

import argparse
import logging
import signal
import threading
from pathlib import Path
from typing import Any, Callable

from flask import Flask, Response, jsonify, render_template
from werkzeug.serving import BaseWSGIServer, make_server

from .camera_service import CameraService, CameraUnavailableError
from .picamera2_backend import Picamera2Backend
from .storage import GIB, MediaStorage, StorageLowError
from host.vision import (
    VisionConfigurationError,
    VisionInitializationError,
    VisionService,
)


LOGGER = logging.getLogger("sentry.camera")
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DATA_DIR = REPOSITORY_ROOT / "data"


def create_app(service: CameraService, vision_service: VisionService) -> Flask:
    app = Flask(__name__)

    def combined_status() -> dict[str, Any]:
        return {**service.status(), **vision_service.status()}

    def success(**values: Any) -> tuple[Response, int]:
        payload = {"ok": True, "status": combined_status(), **values}
        return jsonify(payload), 200

    def run_action(action: Callable[[], Any], response_key: str | None = None):
        try:
            result = action()
            values = {} if response_key is None else {response_key: result}
            return success(**values)
        except CameraUnavailableError as exc:
            return _error_response(combined_status, str(exc), 503)
        except StorageLowError as exc:
            return _error_response(combined_status, str(exc), 507)
        except VisionConfigurationError as exc:
            return _error_response(combined_status, str(exc), 400)
        except VisionInitializationError as exc:
            return _error_response(combined_status, str(exc), 503)
        except (ValueError, TypeError) as exc:
            return _error_response(combined_status, str(exc), 400)
        except Exception as exc:  # keep API failures machine-readable
            LOGGER.exception("camera API action failed")
            return _error_response(combined_status, str(exc), 500)

    @app.get("/")
    def index():
        return render_template("index.html")

    @app.get("/stream")
    def stream():
        if not service.status()["camera_running"]:
            return _error_response(combined_status, "camera is not running", 503)
        return Response(
            _mjpeg_frames(service),
            mimetype="multipart/x-mixed-replace; boundary=FRAME",
            headers={
                "Cache-Control": "no-cache, private",
                "Pragma": "no-cache",
                "Age": "0",
            },
        )

    @app.get("/api/status")
    def status():
        return jsonify(combined_status())

    @app.post("/api/record/start")
    def record_start():
        def action() -> str:
            path = service.start_recording()
            return service.relative_media_path(path)

        return run_action(action, "recording_path")

    @app.post("/api/record/stop")
    def record_stop():
        def action() -> str | None:
            path = service.stop_recording()
            return None if path is None else service.relative_media_path(path)

        return run_action(action, "recording_path")

    @app.post("/api/capture")
    def capture():
        def action() -> str:
            return service.capture("manual").relative_path

        return run_action(action, "capture_path")

    @app.post("/api/dataset/start")
    def dataset_start():
        return run_action(service.start_dataset_capture, "changed")

    @app.post("/api/dataset/stop")
    def dataset_stop():
        return run_action(service.stop_dataset_capture, "changed")

    @app.post("/api/vision/start")
    def vision_start():
        return run_action(vision_service.enable, "changed")

    @app.post("/api/vision/stop")
    def vision_stop():
        return run_action(vision_service.disable, "changed")

    return app


def _error_response(
    status_provider: Callable[[], dict[str, Any]],
    message: str,
    status_code: int,
) -> tuple[Response, int]:
    return (
        jsonify({"ok": False, "error": message, "status": status_provider()}),
        status_code,
    )


def _mjpeg_frames(service: CameraService):
    sequence = 0
    while True:
        try:
            result = service.wait_for_jpeg(sequence, timeout_s=10.0)
        except CameraUnavailableError:
            return
        if result is None:
            if not service.status()["camera_running"]:
                return
            continue
        sequence, frame = result
        yield (
            b"--FRAME\r\n"
            b"Content-Type: image/jpeg\r\n"
            + f"Content-Length: {len(frame)}\r\n\r\n".encode("ascii")
            + frame
            + b"\r\n"
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=_port, default=8080)
    parser.add_argument("--width", type=_positive_even_int, default=1280)
    parser.add_argument("--height", type=_positive_even_int, default=720)
    parser.add_argument("--fps", type=_positive_float, default=30.0)
    parser.add_argument(
        "--capture-interval", type=_positive_float, default=2.0, metavar="SECONDS"
    )
    parser.add_argument("--min-free-gb", type=_nonnegative_float, default=1.0)
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA_DIR)
    parser.add_argument("--model", type=Path)
    parser.add_argument("--target-class", default="animal_mouse")
    parser.add_argument("--confidence", type=_fraction, default=0.5)
    parser.add_argument("--detection-fps", type=_positive_float, default=5.0)
    parser.add_argument("--inference-size", type=_positive_int, default=640)
    parser.add_argument("--dead-zone-x", type=_fraction, default=0.05)
    parser.add_argument("--dead-zone-y", type=_fraction, default=0.05)
    parser.add_argument(
        "--log-level",
        choices=("DEBUG", "INFO", "WARNING", "ERROR"),
        default="INFO",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    storage = MediaStorage(
        args.data_dir,
        int(args.min_free_gb * GIB),
    )
    backend = Picamera2Backend(
        width=args.width,
        height=args.height,
        fps=args.fps,
        logger=LOGGER,
    )
    service = CameraService(
        backend,
        storage,
        width=args.width,
        height=args.height,
        fps=args.fps,
        capture_interval_s=args.capture_interval,
        logger=LOGGER,
    )
    vision_service = VisionService(
        service,
        model_path=args.model,
        target_class=args.target_class,
        confidence=args.confidence,
        detection_fps=args.detection_fps,
        inference_size=args.inference_size,
        dead_zone_x=args.dead_zone_x,
        dead_zone_y=args.dead_zone_y,
        logger=LOGGER,
    )

    server: BaseWSGIServer | None = None
    server_thread: threading.Thread | None = None
    shutdown_event = threading.Event()
    shutdown_error: Exception | None = None

    def request_shutdown(signum: int, _frame: Any) -> None:
        LOGGER.info("received signal %s; shutting down", signum)
        shutdown_event.set()

    old_handlers = {
        signal_number: signal.signal(signal_number, request_shutdown)
        for signal_number in (signal.SIGINT, signal.SIGTERM)
    }

    try:
        service.start()
        vision_service.start()
        app = create_app(service, vision_service)
        server = make_server(args.host, args.port, app, threaded=True)

        def serve() -> None:
            try:
                assert server is not None
                server.serve_forever()
            finally:
                shutdown_event.set()

        server_thread = threading.Thread(
            target=serve,
            name="camera-http-server",
            daemon=False,
        )
        server_thread.start()
        LOGGER.info("camera service listening on http://%s:%d/", args.host, args.port)
        shutdown_event.wait()
    except Exception as exc:
        LOGGER.exception("camera service failed")
        shutdown_error = exc
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()
        if server_thread is not None and server_thread is not threading.current_thread():
            server_thread.join()
        try:
            vision_service.close()
        except Exception as exc:
            LOGGER.exception("vision service shutdown failed")
            shutdown_error = shutdown_error or exc
        try:
            service.close()
        except Exception as exc:
            LOGGER.exception("camera service shutdown failed")
            shutdown_error = shutdown_error or exc
        for signal_number, old_handler in old_handlers.items():
            signal.signal(signal_number, old_handler)

    return 1 if shutdown_error is not None else 0


def _positive_even_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0 or parsed % 2 != 0:
        raise argparse.ArgumentTypeError("must be a positive even integer")
    return parsed


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def _positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def _nonnegative_float(value: str) -> float:
    parsed = float(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return parsed


def _fraction(value: str) -> float:
    parsed = float(value)
    if not 0.0 <= parsed <= 1.0:
        raise argparse.ArgumentTypeError("must be in the range 0..1")
    return parsed


def _port(value: str) -> int:
    parsed = int(value)
    if not 1 <= parsed <= 65535:
        raise argparse.ArgumentTypeError("port must be in the range 1..65535")
    return parsed


if __name__ == "__main__":
    raise SystemExit(main())
