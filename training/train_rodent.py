from ultralytics import YOLO


def main():
    # Start from pretrained COCO weights
    model = YOLO("yolo26n.pt")

    model.train(
        data="data/rodents/data.yaml",
        epochs=100,
        imgsz=640,
        batch=16,
        device=0,
        workers=8,
        project="runs/rodent",
        name="yolo26n_rodent",
    )


if __name__ == "__main__":
    main()