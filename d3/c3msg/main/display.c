#include "display.h"

#include <string.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 规范 RGB565（不预交换），面板需要的字节序/色序在 flush 时统一处理 */
#define P565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static const uint16_t kPal565[C_COUNT] = {
    [C_BG]   = P565(10, 12, 18),
    [C_BAR]  = P565(22, 26, 38),
    [C_LINE] = P565(44, 52, 70),
    [C_FG]   = P565(226, 232, 240),
    [C_DIM]  = P565(132, 146, 168),
    [C_IN]   = P565(120, 230, 160),
    [C_OUT]  = P565(120, 200, 255),
    [C_WARN] = P565(255, 196, 96),
    [C_ERR]  = P565(255, 110, 110),
    [C_SEL]  = P565(30, 60, 96),
    [C_BOARD]   = P565(226, 200, 150),   /* 与 gomoku C_WOOD(0xE79C) 同色系 */
    [C_STONE_B] = P565(16, 16, 20),
    [C_STONE_W] = P565(245, 245, 245),
    [C_CYAN]    = P565(80, 220, 230),
    [C_MAGENTA] = P565(240, 110, 220),
    [C_ORANGE]  = P565(255, 150, 60),
    [C_RGB_R]   = 0xF800,
    [C_RGB_G]   = 0x07E0,
    [C_RGB_B]   = 0x001F,
};

/* 小屏：ST7735S 80x160，物理竖屏 + 软件旋转 + INVON + BGR
 * off_x=26 / off_y=1 为 2026-11-15 实机目视确定：off_y=0 会在横屏最左留一条
 * 从未刷新的竖线（可见行是 1..160，写 0..159 会漏掉一整行）。 */
static const panel_profile_t kSmall = {
    .name = "ST7735S 80x160",
    .phys_w = 80, .phys_h = 160,
    .scr_w = 160, .scr_h = 80,
    .off_x = 26, .off_y = 1,
    .madctl = 0x00,
    .invert = true,
    .swap_rb = true,
    .sw_rotate = true,
};

/* 大屏：ST7789 320x240，参数与 zd/gomoku_c3 实机验证过的 st7789_init 一致 */
static const panel_profile_t kLarge = {
    .name = "ST7789 320x240",
    .phys_w = 320, .phys_h = 240,
    .scr_w = 320, .scr_h = 240,
    .off_x = 0, .off_y = 0,
    .madctl = 0x60,          /* MX|MV：横屏 */
    .invert = false,         /* INVOFF(0x20) */
    .swap_rb = false,        /* 正常 RGB 顺序 */
    .sw_rotate = false,      /* 方向由 MADCTL 完成，不再软件旋转 */
};

const panel_profile_t *g_panel = &kSmall;
int g_scr_w = 160;
int g_scr_h = 80;

static spi_device_handle_t s_spi;       /* 40 MHz，刷屏/LoRa 共用总线 */
static spi_device_handle_t s_spi_slow;  /* 1 MHz，仅开机读屏 ID */
static uint8_t *s_fb;                   /* 物理顺序的 8bit 调色板帧 */
static uint8_t s_chunk[320 * 2 * 4];    /* 4 行 565 */
static int s_off_x, s_off_y;            /* 当前生效的窗口偏移 */

static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_LCD_DC, 0);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_data(const uint8_t *d, int n)
{
    if (n <= 0) return;
    gpio_set_level(PIN_LCD_DC, 1);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = n * 8;
    t.tx_buffer = d;
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_cmd1(uint8_t cmd, uint8_t v)
{
    lcd_cmd(cmd);
    lcd_data(&v, 1);
}

static void lcd_window(int x0, int y0, int x1, int y1)
{
    uint8_t b[4];
    b[0] = (uint8_t)(x0 >> 8); b[1] = (uint8_t)x0;
    b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
    lcd_cmd(0x2A);
    lcd_data(b, 4);
    b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0;
    b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
    lcd_cmd(0x2B);
    lcd_data(b, 4);
    lcd_cmd(0x2C);
}

/* RDDID(0x04)，1 MHz；读不到返回 0 */
static int panel_read_id(uint8_t out[3])
{
    uint8_t cmd = 0x04;
    spi_transaction_t t;
    spi_device_acquire_bus(s_spi_slow, portMAX_DELAY);
    gpio_set_level(PIN_LCD_DC, 0);
    gpio_set_level(PIN_LCD_CS, 0);
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_polling_transmit(s_spi_slow, &t);
    gpio_set_level(PIN_LCD_DC, 1);
    memset(&t, 0, sizeof(t));
    t.flags = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA;
    t.length = 24;
    t.rxlength = 24;
    memset(t.tx_data, 0xFF, sizeof(t.tx_data));
    spi_device_polling_transmit(s_spi_slow, &t);
    gpio_set_level(PIN_LCD_CS, 1);
    spi_device_release_bus(s_spi_slow);
    memcpy(out, t.rx_data, 3);
    return (out[0] | out[1] | out[2]) != 0;
}

static const panel_profile_t *pick_profile(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static const uint8_t kLargeMac[6] = {0x50, 0x78, 0x7D, 0xF2, 0xAA, 0xD0};

    uint8_t id[3] = {0};
    int alive = panel_read_id(id);
    printf("panel probe: RDDID=%02X %02X %02X -> %s\n", id[0], id[1], id[2],
           alive ? "id readable" : "no id (panel SDO not wired)");

    if (alive) {
        if (id[0] == 0x85) return &kLarge;          /* ST7789 */
        if (id[0] == 0x7C) return &kSmall;          /* ST7735S */
    }
    /* 这块板的面板 SDO 没接，读不到 ID：按 MAC 选已实机验证过的 profile */
    if (memcmp(mac, kLargeMac, 6) == 0) return &kLarge;
    return &kSmall;
}

void display_init(void)
{
    gpio_config_t io;
    memset(&io, 0, sizeof(io));
    io.pin_bit_mask = (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_BL);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(PIN_LCD_CS, 1);
    gpio_set_level(PIN_LCD_DC, 1);
    gpio_set_level(PIN_LCD_BL, 0);   /* 与 gomoku 一致：init 期间背光关，init 完再开 */

    spi_bus_config_t buscfg;
    memset(&buscfg, 0, sizeof(buscfg));
    buscfg.mosi_io_num = PIN_MOSI;
    buscfg.miso_io_num = PIN_MISO;
    buscfg.sclk_io_num = PIN_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 5120;   /* gomoku 用 LCD_W*8*2；4 行 320 像素只需 2560 */
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg;
    memset(&devcfg, 0, sizeof(devcfg));
    devcfg.clock_speed_hz = 40 * 1000 * 1000;
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;
    devcfg.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi));

    spi_device_interface_config_t slowcfg;
    memset(&slowcfg, 0, sizeof(slowcfg));
    slowcfg.clock_speed_hz = 1000 * 1000;
    slowcfg.mode = 0;
    slowcfg.spics_io_num = -1;
    slowcfg.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &slowcfg, &s_spi_slow));

    g_panel = pick_profile();
    g_scr_w = g_panel->scr_w;
    g_scr_h = g_panel->scr_h;
    s_off_x = g_panel->off_x;
    s_off_y = g_panel->off_y;

    s_fb = heap_caps_malloc((size_t)g_panel->phys_w * g_panel->phys_h, MALLOC_CAP_8BIT);
    if (!s_fb) {
        printf("display: framebuffer alloc failed (%d x %d)\n", g_panel->phys_w, g_panel->phys_h);
    } else {
        memset(s_fb, C_BG, (size_t)g_panel->phys_w * g_panel->phys_h);
    }

    /* 与 gomoku_c3 实机可用序列一致：SWRESET/SLPOUT/COLMOD/MADCTL/INV/NORON/DISPON */
    spi_device_acquire_bus(s_spi, portMAX_DELAY);
    gpio_set_level(PIN_LCD_CS, 0);
    lcd_cmd(0x01);
    spi_device_release_bus(s_spi);
    gpio_set_level(PIN_LCD_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    spi_device_acquire_bus(s_spi, portMAX_DELAY);
    gpio_set_level(PIN_LCD_CS, 0);
    lcd_cmd(0x11);
    spi_device_release_bus(s_spi);
    gpio_set_level(PIN_LCD_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_device_acquire_bus(s_spi, portMAX_DELAY);
    gpio_set_level(PIN_LCD_CS, 0);
    lcd_cmd1(0x3A, 0x55);                    /* 16bit */
    lcd_cmd1(0x36, g_panel->madctl);
    lcd_cmd(g_panel->invert ? 0x21 : 0x20);
    lcd_cmd(0x13);                           /* NORON */
    lcd_cmd(0x29);                           /* DISPON */
    gpio_set_level(PIN_LCD_CS, 1);
    spi_device_release_bus(s_spi);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_BL, 1);   /* init 完毕，开背光 */

    printf("display: %s logical=%dx%d off=%d,%d madctl=%02X inv=%d swap_rb=%d rot=%d fb=%uKB heap=%uKB\n",
           g_panel->name, g_scr_w, g_scr_h, s_off_x, s_off_y,
           g_panel->madctl, (int)g_panel->invert, (int)g_panel->swap_rb, (int)g_panel->sw_rotate,
           (unsigned)((size_t)g_panel->phys_w * g_panel->phys_h / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
    fflush(stdout);
}

void display_put(int x, int y, color_t c)
{
    if (!s_fb) return;
    if (x < 0 || y < 0 || x >= g_scr_w || y >= g_scr_h) return;
    if (g_panel->sw_rotate) {
        int px = y;
        int py = g_panel->phys_h - 1 - x;
        s_fb[py * g_panel->phys_w + px] = c;
    } else {
        s_fb[y * g_panel->phys_w + x] = c;
    }
}

color_t display_get(int x, int y)
{
    if (!s_fb) return C_BG;
    if (x < 0 || y < 0 || x >= g_scr_w || y >= g_scr_h) return C_BG;
    if (g_panel->sw_rotate) {
        int px = y;
        int py = g_panel->phys_h - 1 - x;
        return s_fb[py * g_panel->phys_w + px];
    }
    return s_fb[y * g_panel->phys_w + x];
}

void display_fill(color_t c)
{
    if (!s_fb) return;
    memset(s_fb, c, (size_t)g_panel->phys_w * g_panel->phys_h);
}

void display_set_offsets(int off_x, int off_y)
{
    s_off_x = off_x;
    s_off_y = off_y;
    printf("display: offsets -> %d,%d\n", s_off_x, s_off_y);
    fflush(stdout);
}

void display_flush(void)
{
    if (!s_fb) return;
    const int w = g_panel->phys_w;
    const int h = g_panel->phys_h;
    const int x0 = s_off_x;
    const int y0 = s_off_y;

    spi_device_acquire_bus(s_spi, portMAX_DELAY);
    gpio_set_level(PIN_LCD_CS, 0);
    lcd_window(x0, y0, x0 + w - 1, y0 + h - 1);

    /* 关键：0x2C 是命令，DC 还是低。发像素前必须拉高 DC，
     * 否则整帧 565 被当作命令解析（现象：首帧花屏、随后白屏）。
     * gomoku_c3 的 push_screen() 就是在这里 gpio_set_level(PIN_DC, 1)。 */
    gpio_set_level(PIN_LCD_DC, 1);

    for (int y = 0; y < h; y += 4) {
        int rows = (y + 4 <= h) ? 4 : (h - y);
        int k = 0;
        for (int yy = y; yy < y + rows; yy++) {
            const uint8_t *row = s_fb + (size_t)yy * w;
            for (int x = 0; x < w; x++) {
                uint16_t c = kPal565[row[x] < C_COUNT ? row[x] : C_BG];
                if (g_panel->swap_rb) {
                    c = (uint16_t)(((c & 0x001F) << 11) | (c & 0x07E0) | ((c & 0xF800) >> 11));
                }
                s_chunk[k++] = (uint8_t)(c >> 8);   /* MSB first，与 gomoku 一致 */
                s_chunk[k++] = (uint8_t)(c & 0xFF);
            }
        }
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = k * 8;
        t.tx_buffer = s_chunk;
        spi_device_polling_transmit(s_spi, &t);
    }
    gpio_set_level(PIN_LCD_CS, 1);
    spi_device_release_bus(s_spi);
}

void display_rgb_cycle(void)
{
    static const color_t kSeq[3] = { C_RGB_R, C_RGB_G, C_RGB_B };
    static const char *kName[3] = { "RED", "GREEN", "BLUE" };
    int i = 0;
    while (1) {
        display_fill(kSeq[i]);
        for (int k = 0; k < 4; k++) {          /* 2s，期间反复整帧重推 */
            if (k == 0) {
                printf("RGB TEST %s\n", kName[i]);
                fflush(stdout);
            }
            display_flush();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        i = (i + 1) % 3;
    }
}

void display_dump(const char *tag)
{
    printf("@FBRLE %s %d %d %d\n", tag, g_scr_w, g_scr_h, g_panel->swap_rb ? 1 : 0);
    int run = 0;
    uint16_t last = 0;
    for (int y = 0; y < g_scr_h; y++) {
        for (int x = 0; x < g_scr_w; x++) {
            color_t idx = display_get(x, y);
            uint16_t c = kPal565[idx < C_COUNT ? idx : C_BG];
            if (g_panel->swap_rb) {
                c = (uint16_t)(((c & 0x001F) << 11) | (c & 0x07E0) | ((c & 0xF800) >> 11));
            }
            if (run && c == last && run < 65535) { run++; continue; }
            if (run) printf("%d %04X\n", run, last);
            last = c;
            run = 1;
        }
    }
    if (run) printf("%d %04X\n", run, last);
    printf("@END\n");
    fflush(stdout);
}
