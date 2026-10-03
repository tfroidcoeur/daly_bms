# Porting to other hardware

`core/` and `ui/` are platform-free: plain C99 and LVGL 9. Any board that can
run LVGL, drive a display and send and receive 29-bit CAN frames at 250 kbit/s
can host this monitor. A port supplies five things, and nothing else.

The worked example throughout is the reference port in `port/esp32/`:
**Waveshare ESP32-S3-RLCD-4.2** (ESP32-S3, FreeRTOS via ESP-IDF 5.x, ST7305
4.2" reflective LCD) with an **SN65HVD230** transceiver on the S3's built-in
TWAI controller. `port/sim/` is a second, much smaller port (Linux SocketCAN +
SDL) and a good template.

## 1. A millisecond clock

`poller_tick()` and `poller_on_frame()` take `now_ms` as a `uint32_t`. Any
monotonic millisecond counter works; wrapping is handled.

*Example:* `esp_timer_get_time() / 1000` in `port/esp32/main/main.c`.

## 2. A CAN link

Four operations, all non-blocking:

| Operation | Notes |
|---|---|
| send an extended frame | must not block the loop on a stalled bus; dropping is fine |
| receive an extended frame | return "empty" only when the queue is empty; skip standard-ID frames |
| filter (optional) | accept priority `0x18` with destination `0x40`; also the virtual BMS address if enabled |
| bus-off recovery | the bus must come back by itself after a fault |

Any controller fits: an on-chip one (ESP32 TWAI, STM32 bxCAN/FDCAN, NXP
FlexCAN), an SPI controller such as the MCP2515, or SocketCAN on Linux. The
transceiver only needs to match the controller's I/O voltage.

The main loop is the same on every platform:

```c
can_frame_out_t tx;
if (poller_tick(&poller, now, &tx)) {
    link_send(tx.id, tx.data, tx.len);
}
while (link_recv(&id, data, &len)) {
    poller_on_frame(&poller, id, data, len, now);
    n = vbms_on_frame(&vbms, &model, id, data, len, answer);   /* optional */
    for (i = 0; i < n; i++) link_send(answer[i].id, answer[i].data, answer[i].len);
}
```

Size the receive queue for a 0x95 burst (8 frames for 24 cells, up to 16), and
the transmit queue for a virtual-BMS answer of the same size.

*Example:* `port/esp32/main/twai_link.c`. The S3 has a single TWAI controller,
so all three packs share one bus and are told apart by Daly address.

## 3. A display

Create an LVGL display of `UI_HOR_RES` x `UI_VER_RES` (400 x 300) and give it a
flush callback that writes to your panel.

The UI is designed for a **monochrome** panel. LVGL renders RGB565 and both
existing ports reduce each pixel with `ui_px_is_paper()` from `ui/ui.h`. A
colour panel can take the RGB565 output as is. A different resolution needs
layout work in `ui/page_*.c`: the pages are laid out for 400 x 300 and check
their text widths at compile time.

The pack count is `BMS_PACK_COUNT` in `core/bms_model.h`; `core/` follows it,
but the UI has exactly three pack pages, so a different count needs UI work too.

*Example:* `board_display_init()` in `port/esp32/main/board.c`. The ST7305
packs 4 x 2 pixel blocks per byte and scans portrait, so `flush_cb` rotates
and sets pixels one at a time through `st7305_set_pixel()`.

## 4. Input

Two events, `ui_input(UI_KEY_SHORT)` and `ui_input(UI_KEY_LONG)`: next page,
and open/close the cell detail. Any buttons, pads or encoder will do.

*Example:* `board_key_poll()` in `port/esp32/main/board.c` debounces two
capacitive pads read by the S3 touch peripheral, plus the onboard KEY button
(short press / 800 ms hold).

## 5. Tasks and a lock (on an RTOS)

Rendering a frame can take long enough to starve the CAN loop, so under
FreeRTOS (or any RTOS) run LVGL in its own task and guard it:

| Task | Does |
|---|---|
| poll | CAN, input, bus health; pushes the model into the UI under the lock |
| LVGL | `lv_timer_handler()`: rendering and the flush |

- Use a recursive mutex around every LVGL call.
- The poll task never waits long for it: try with a short timeout and retry next
  pass, rather than miss CAN frames.
- Latch input events until the lock is taken, so no press is lost.
- `ui_init()` may run unlocked before the LVGL task starts.
- `core/` touches no LVGL state and needs no lock.

A single-threaded port (the simulator) just calls `lv_timer_handler()` from its
loop.

*Example:* `board_lvgl_start()`, `board_lvgl_lock()`/`board_lvgl_unlock()` in
`port/esp32/main/board.c`; the poll task is `app_main` on core 0, LVGL runs on
core 1 at higher priority.

## Build

Compile `core/*.c`, `ui/*.c` and `ui/fonts/*.c` with your port, with
`LV_LVGL_H_INCLUDE_SIMPLE` defined and LVGL 9.5.0. Set `DALY_CURRENT_SIGN` to
`-1` if your packs report discharge above the 30000 current bias (see
`core/daly_proto.h`). `port/esp32/main/CMakeLists.txt` and the top-level
`CMakeLists.txt` are both short examples.
