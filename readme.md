# Terminal Audio Visualizer

A real-time terminal audio spectrum visualizer written in C using **miniaudio**, **KissFFT**, and **ncurses**.

## Build & Run

Make sure `miniaudio.h`, `kiss_fft.h`, and `kiss_fft.c` are in your project folder (or installed on your system), then compile with:

```bash
gcc advzr.c kiss_fft.c -o visualizer -lncurses -lm -lpthread -ldl
```

Run it with an audio file (such as `.mp3`):

```bash
./visualizer song.mp3
```

Press **`q`** or **`Esc`** to quit.

---

## Customization

### 1. Change the Bar Symbol
Open `advzr.c` and look around **line 191**:

```c
const char symbols[] = "\$";
```

* **Single character:** Replace `"$"` with whatever character you want (e.g., `"#"` or `"|"`).
* **Random mix of characters:** Put multiple characters inside the quotes (e.g., `"#$*+"`), and the visualizer will randomly pick one for every block drawn.
* *(Optional)* If your terminal doesn't support colors, change `'#'` on **line 198** (`mvaddch(y, x, '#');`) as well.

### 2. Change the Bar Speed (Refresh Rate & Smoothing)
There are two places in `advzr.c` that control how fast the bars move:

* **Frame Rate (Line 211):**
  ```c
  ma_sleep(8);
  ```
  This pauses the loop for `8` milliseconds per frame (~120 FPS).
  * **Lower number** (e.g., `4`) = faster updates.
  * **Higher number** (e.g., `30`) = slower, choppier updates.

* **Bar Reaction / Fall Speed (Lines 172–176):**
  ```c
  if (target_height > smoothed_heights[bar]) {
      smoothed_heights[bar] = target_height * 0.72f + smoothed_heights[bar] * 0.28f;
  } else {
      smoothed_heights[bar] = smoothed_heights[bar] * 0.82f + target_height * 0.18f;
  }
  ```
  * The **`if` block** controls how fast bars jump up (`0.72f` = snappy rise).
  * The **`else` block** controls how fast bars fall back down (`0.82f` = smooth decay). Lowering `0.82f` (e.g., to `0.50f`) makes the bars drop much faster.

> **Note:** Recompile with the `gcc` command above after making any edits to `advzr.c`.
