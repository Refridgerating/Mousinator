from ultralytics import YOLO
import torch


def main():
    device = 0 if torch.cuda.is_available() else "cpu"

    print(f"Training device: {device}")

    if torch.cuda.is_available():
        print(f"GPU: {torch.cuda.get_device_name(0)}")

    model = YOLO("yolo26n.pt")

    model.train(
        data="data/mouse_dataset/yolo_all_v1/data.yaml",

        epochs=150,
        imgsz=640,
        batch=-1,
        device=device,
        workers=8,

        pretrained=True,

        # Important:
        # COCO "mouse" = computer mouse.
        # Do not remap that class into animal_mouse.
        cls_remap=False,

        optimizer="auto",

        # Use every image for training.
        # There is intentionally no validation set in this run.
        val=False,

        close_mosaic=10,

        project="runs",
        name="animal_mouse_all_v1",

        save=True,
        plots=True,
    )


if __name__ == "__main__":
    main()