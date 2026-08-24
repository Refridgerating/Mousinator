from pathlib import Path
import shutil

import cv2


SOURCE = Path("data/mouse_dataset/raw/positives")
REJECTED = Path("data/mouse_dataset/raw/rejected")

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp"}


def load_images():
    return sorted(
        p for p in SOURCE.iterdir()
        if p.suffix.lower() in IMAGE_EXTENSIONS
    )


def fit_image(img, max_width=1400, max_height=900):
    h, w = img.shape[:2]

    scale = min(
        max_width / w,
        max_height / h,
        1.0,
    )

    if scale < 1:
        img = cv2.resize(
            img,
            (int(w * scale), int(h * scale)),
            interpolation=cv2.INTER_AREA,
        )

    return img


def main():
    REJECTED.mkdir(parents=True, exist_ok=True)

    images = load_images()

    if not images:
        print("No images found.")
        return

    index = 0

    print(
        "\nControls:\n"
        "  SPACE / D  = keep and next\n"
        "  A          = previous\n"
        "  X          = reject and move to rejected/\n"
        "  Q          = quit\n"
    )

    while 0 <= index < len(images):

        path = images[index]

        if not path.exists():
            index += 1
            continue

        img = cv2.imread(str(path))

        if img is None:
            print(f"Unreadable: {path}")
            shutil.move(str(path), REJECTED / path.name)
            index += 1
            continue

        img = fit_image(img)

        text = f"{index + 1}/{len(images)}   {path.name}"

        cv2.putText(
            img,
            text,
            (20, 35),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (255, 255, 255),
            2,
            cv2.LINE_AA,
        )

        cv2.imshow("Mouse Dataset Review", img)

        key = cv2.waitKey(0) & 0xFF

        # keep / next
        if key in (ord("d"), ord(" "), 13):
            index += 1

        # previous
        elif key == ord("a"):
            index = max(0, index - 1)

        # reject
        elif key == ord("x"):
            destination = REJECTED / path.name

            shutil.move(str(path), destination)

            print(f"Rejected: {path.name}")

            images.pop(index)

            if index >= len(images):
                index = len(images) - 1

        # quit
        elif key == ord("q"):
            break

    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()