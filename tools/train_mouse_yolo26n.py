#!/usr/bin/env python3
from ultralytics import YOLO
import torch


def main():
    device = 0 if torch.cuda.is_available() else "cpu"

    print(f"Training device: {device}")
    if torch.cuda.is_available():
        print(f"GPU: {torch.cuda.get_device_name(0)}")

    # Fine-tune from pretrained YOLO26n weights.
    model = YOLO("yolo26n.pt")

    model.train(
        data="data/mouse_dataset/yolo_v1/data.yaml",
        epochs=100,
        imgsz=640,
        batch=-1,          # Ultralytics auto-batch
        device=device,
        workers=8,
        patience=25,
        project="runs/mouse",
        name="yolo26n_mouse_v1",
        pretrained=True,
        optimizer="auto",
        close_mosaic=10,
        plots=True,
        save=True,
    )

    print()
    print("Training complete.")
    print("Best weights:")
    print("runs/mouse/yolo26n_mouse_v1/weights/best.pt")
    print()
    print("Keep the test split untouched until model/hyperparameter choices are settled.")


if __name__ == "__main__":
    main()
