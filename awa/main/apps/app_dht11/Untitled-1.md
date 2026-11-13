02:49:03 [🤖 系统] 连接成功！等待数据...
I (290) esp_image: segment 1: paddr=001bf71c vaddr=3fc9c800 size[628] [[32minfo[0m] [SystemUtils] system utils init

[639] [[32minfo[0m] [DisplayManager] Canvas reset complete: valid=%d

[0;32mI (1139) Settings: SD settings not found: /sdcard/system/system.json[0m

[639] [[32minfo[0m] [Input] input init

[639] [[32minfo[0m] [Keyboard] init - FIX V10 (Global Lock)

[643] [[32minfo[0m] [Power] power init

[643] [[32minfo[0m] [Network] network init

[643] [[32minfo[0m] [Storage] storage init

[643] [[32minfo[0m] [LifeGuard] GuardData initialized (Safe-Heap)

[644] [[32minfo[0m] [System] system init

[644] [[32minfo[0m] [UIRenderer] ui renderer init

[644] [[32minfo[0m] [HalFacade] all modules initialized

[645] [[32minfo[0m] [ConfigStore] Config loaded: provider=deepseek model=deepseek-chat

[645] [[32minfo[0m] [ModelRegistry] Registered 20 models

[646] [[32minfo[0m] [AiEngine] init: ready=false

[646] [[32minfo[0m] [ChatEngine] init

[646] [[32minfo[0m] [ChatEngine] New conversation: 00000000f42d

[647] [[32minfo[0m] [UiEngine] init

[660] [[32minfo[0m] [SdCard] Initializing SD card...

[0;31mE (1283) sdmmc_sd: sdmmc_init_sd_if_cond: send_if_cond (1) returned 0x108[0m

[0;31mE (1283) vfs_fat_sdmmc: sdmmc_card_init failed (0x108).[0m

[784] [[31merror[0m] [SdCard] SD mount failed: 264

[784] [[32minfo[0m] [ToolMgr] init

[785] [[32minfo[0m] [MCP] init

[785] [[32minfo[0m] [Main] First boot - setup mode

[785] [[32minfo[0m] [Main] System init done, state=1



assert failed: tcpip_send_msg_wait_sem /IDF/components/lwip/lwip/src/api/tcpip.c:454 (Invalid mbox)





Backtrace: 0x40381489:0x3fcb26b0 0x40381451:0x3fcb26d0 0x40389a39:0x3fcb26f0 0x4208ccd3:0x3fcb2810 0x4209ed66:0x3fcb2840 0x4209edfa:0x3fcb2860 0x4208c321:0x3fcb28b0 0x4204e8ea:0x3fcb28d0 0x4204efe0:0x3fcb2930 0x420143e5:0x3fcb2960 0x4200af55:0x3fcb2a10 0x4200b978:0x3fcb2a90 0x40382199:0x3fcb2ab0









ELF file SHA256: a7a40d7db



Rebooting...

ESP-ROM:esp32s3-20210327

Build:Mar 27 2021

rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)

Saved PC:0x40381608

SPIWP:0xee

mode:DIO, clock div:1

load:0x3fce2820,len:0x1700

load:0x403c8700,len:0xe1c

load:0x403cb700,len:0x31cc

entry 0x403c8938

I (24) boot: ESP-IDF v5.5.1-dirty 2nd stage bootloader

I (24) boot: compile time Apr 28 2026 00:52:34

I (24) boot: Multicore bootloader

I (25) boot: chip revision: v0.2

I (25) boot: efuse block revision: v1.3

I (25) qio_mode: Enabling default flash chip QIO

I (25) boot.esp32s3: Boot SPI Speed : 80MHz

I (25) boot.esp32s3: SPI Mode       : QIO

I (25) boot.esp32s3: SPI Flash Size : 8MB

I (26) boot: Enabling RNG early entropy source...

I (26) boot: Partition Table:

I (26) boot: ## Label            Usage          Type ST Offset   Length

I (26) boot:  0 nvs              WiFi data        01 02 00009000 00004000

I (27) boot:  1 otadata          OTA data         01 00 0000d000 00002000

I (27) boot:  2 phy_init         RF data          01 01 0000f000 00001000

I (28) boot:  3 factory          factory app      00 00 00010000 00400000

I (28) boot:  4 storage          Unknown data     01 82 00410000 00380000

I (28) boot: End of partition table

I (29) boot: Defaulting to factory image

I (29) esp_image: segment 0: paddr=00010020 vaddr=3c120020 size=1af6f4h (1767156) map

I (291) esp_image: segment 1: paddr=001bf71c vaddr=3fc9c800 size=008fch (  2300) load

I (292) esp_image: segment 2: paddr=001c0020 vaddr=42000020 size=1134a0h (1127584) map

I (460) esp_image: segment 3: paddr=002d34c8 vaddr=3fc9d0fc size=04710h ( 18192) load

I (463) esp_image: segment 4: paddr=002d7be0 vaddr=40378000 size=14784h ( 83844) load

I (480) esp_image: segment 5: paddr=002ec36c vaddr=50000000 size=00020h (    32) load

I (488) boot: Loaded app from partition at offset 0x10000

I (488) boot: Disabling RNG early entropy source...

[0;32mI (489) cpu_start: Multicore app[0m

[0;32mI (498) cpu_start: Pro cpu start user code[0m

[0;32mI (498) cpu_start: cpu freq: 240000000 Hz[0m

[0;32mI (498) app_init: Application information:[0m

[0;32mI (498) app_init: Project name:     cardputer-adv[0m

[0;32mI (499) app_init: App version:      1[0m

[0;32mI (499) app_init: Compile time:     Apr 28 2026 00:52:15[0m

[0;32mI (499) app_init: ELF file SHA256:  a7a40d7db...[0m

[0;32mI (499) app_init: ESP-IDF:          v5.5.1-dirty[0m

[0;32mI (499) efuse_init: Min chip rev:     v0.0[0m

[0;32mI (499) efuse_init: Max chip rev:     v0.99 [0m

[0;32mI (500) efuse_init: Chip rev:         v0.2[0m

[0;32mI (500) heap_init: Initializing. RAM available for dynamic allocation:[0m

[0;32mI (500) heap_init: At 3FCA7AF0 len 00041C20 (263 KiB): RAM[0m

[0;32mI (500) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM[0m

[0;32mI (501) heap_init: At 600FE000 len 00001FE8 (7 KiB): RTCRAM[0m

[0;32mI (501) spi_flash: detected chip: gd[0m

[0;32mI (501) spi_flash: flash io: qio[0m

[0;32mI (502) sleep_gpio: Configure to isolate all GPIO pins in sleep state[0m

[0;32mI (503) sleep_gpio: Enable automatic switching of GPIO sleep configuration[0m

[0;32mI (503) coexist: coex firmware version: b0bcc39[0m

[0;32mI (504) coexist: coexist rom version e7ae62f[0m

[0;32mI (504) main_task: Started on CPU0[0m

[0;32mI (505) main_task: Calling app_main()[0m

[1315] [[32minfo[0m] [Main] === Cardputer ADV AI Assistant v1.0 ===

[1315] [[32minfo[0m] [HalFacade] creating hal facade instance

[1316] [[32minfo[0m] [HalFacade] hal facade init

[0;32mI (528) M5GFX: [Autodetect] board_M5CardputerADV[0m

[1889] [[32minfo[0m] [SystemUtils] system utils init

[1900] [[32minfo[0m] [DisplayManager] Canvas reset complete: valid=%d

[0;32mI (1090) Settings: SD settings not found: /sdcard/system/system.json[0m

[1900] [[32minfo[0m] [Input] input init

[1901] [[32minfo[0m] [Keyboard] init - FIX V10 (Global Lock)

[1904] [[32minfo[0m] [Power] power init

[1904] [[32minfo[0m] [Network] network init

[1904] [[32minfo[0m] [Storage] storage init

[1905] [[32minfo[0m] [LifeGuard] GuardData initialized (Safe-Heap)

[1905] [[32minfo[0m] [System] system init

[1905] [[32minfo[0m] [UIRenderer] ui renderer init

[1905] [[32minfo[0m] [HalFacade] all modules initialized

[1906] [[32minfo[0m] [ConfigStore] Config loaded: provider=deepseek model=deepseek-chat

[1907] [[32minfo[0m] [ModelRegistry] Registered 20 models

[1907] [[32minfo[0m] [AiEngine] init: ready=false

[1907] [[32minfo[0m] [ChatEngine] init

[1908] [[32minfo[0m] [ChatEngine] New conversation: 00000001f42d

[1908] [[32minfo[0m] [UiEngine] init

[1922] [[32minfo[0m] [SdCard] Initializing SD card...

[0;31mE (1314) sdmmc_sd: sdmmc_init_sd_if_cond: send_if_cond (1) returned 0x108[0m

[0;31mE (1314) vfs_fat_sdmmc: sdmmc_card_init failed (0x108).[0m

[2124] [[31merror[0m] [SdCard] SD mount failed: 264

[2125] [[32minfo[0m] [ToolMgr] init

[2125] [[32minfo[0m] [MCP] init

[2125] [[32minfo[0m] [Main] First boot - setup mode

[2125] [[32minfo[0m] [Main] System init done, state=1



assert failed: tcpip_send_msg_wait_sem /IDF/components/lwip/lwip/src/api/tcpip.c:454 (Invalid mbox)





Backtrace: 0x40381489:0x3fcb26b0 0x40381451:0x3fcb26d0 0x40389a39:0x3fcb26f0 0x4208ccd3:0x3fcb2810 0x4209ed66:0x3fcb2840 0x4209edfa:0x3fcb2860 0x4208c321:0x3fcb28b0 0x4204e8ea:0x3fcb28d0 0x4204efe0:0x3fcb2930 0x420143e5:0x3fcb2960 0x4200af55:0x3fcb2a10 0x4200b978:0x3fcb2a90 0x40382199:0x3fcb2ab0









ELF file SHA256: a7a40d7db



Rebooting...

ESP-ROM:esp32s3-20210327

Build:Mar 27 2021

rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)

Saved PC:0x40381608

SPIWP:0xee

mode:DIO, clock div:1

load:0x3fce2820,len:0x1700

load:0x403c8700,len:0xe1c

load:0x403cb700,len:0x31cc

entry 0x403c8938

I (24) boot: ESP-IDF v5.5.1-dirty 2nd stage bootloader

I (24) boot: compile time Apr 28 2026 00:52:34

I (24) boot: Multicore bootloader

I (24) boot: chip revision: v0.2

I (25) boot: efuse block revision: v1.3

I (25) qio_mode: Enabling default flash chip QIO

I (25) boot.esp32s3: Boot SPI Speed : 80MHz

I (25) boot.esp32s3: SPI Mode       : QIO

I (26) boot.esp32s3: SPI Flash Size : 8MB

I (26) boot: Enabling RNG early entropy source...

I (27) boot: Partition Table:

I (27) boot: ## Label            Usage          Type ST Offset   Length

I (27) boot:  0 nvs              WiFi data        01 02 00009000 00004000

I (28) boot:  1 otadata          OTA data         01 00 0000d000 00002000

I (28) boot:  2 phy_init         RF data          01 01 0000f000 00001000

I (29) boot:  3 factory          factory app      00 00 00010000 00400000

I (29) boot:  4 storage          Unknown data     01 82 00410000 00380000

I (29) boot: End of partition table

I (30) boot: Defaulting to factory image

I (30) esp_image: segment 0: paddr=00010020 vaddr=3c120020 size=1af6f4h (1767156) map

I (292) esp_image: segment 1: paddr=001bf71c vaddr=3fc9c800 size=008fch (  2300) load

I (293) esp_image: segment 2: paddr=001c0020 vaddr=42000020 size=1134a0h (1127584) map

I (461) esp_image: segment 3: paddr=002d34c8 vaddr=3fc9d0fc size=04710h ( 18192) load

I (465) esp_image: segment 4: paddr=002d7be0 vaddr=40378000 size=14784h ( 83844) load

I (481) esp_image: segment 5: paddr=002ec36c vaddr=50000000 size=00020h (    32) load

I (489) boot: Loaded app from partition at offset 0x10000

I (490) boot: Disabling RNG early entropy source...

[0;32mI (490) cpu_start: Multicore app[0m

[0;32mI (499) cpu_start: Pro cpu start user code[0m

[0;32mI (499) cpu_start: cpu freq: 240000000 Hz[0m

[0;32mI (499) app_init: Application information:[0m

[0;32mI (500) app_init: Project name:     cardputer-adv[0m

[0;32mI (500) app_init: App version:      1[0m

[0;32mI (500) app_init: Compile time:     Apr 28 2026 00:52:15[0m

[0;32mI (500) app_init: ELF file SHA256:  a7a40d7db...[0m

[0;32mI (500) app_init: ESP-IDF:          v5.5.1-dirty[0m

[0;32mI (501) efuse_init: Min chip rev:     v0.0[0m

[0;32mI (501) efuse_init: Max chip rev:     v0.99 [0m

[0;32mI (501) efuse_init: Chip rev:         v0.2[0m

[0;32mI (501) heap_init: Initializing. RAM available for dynamic allocation:[0m

[0;32mI (501) heap_init: At 3FCA7AF0 len 00041C20 (263 KiB): RAM[0m

[0;32mI (502) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM[0m

[0;32mI (502) heap_init: At 600FE000 len 00001FE8 (7 KiB): RTCRAM[0m

[0;32mI (503) spi_flash: detected chip: gd[0m

[0;32mI (503) spi_flash: flash io: qio[0m

[0;32mI (504) sleep_gpio: Configure to isolate all GPIO pins in sleep state[0m

[0;32mI (504) sleep_gpio: Enable automatic switching of GPIO sleep configuration[0m

[0;32mI (505) coexist: coex firmware version: b0bcc39[0m

[0;32mI (505) coexist: coexist rom version e7ae62f[0m

[0;32mI (506) main_task: Started on CPU0[0m

[0;32mI (507) main_task: Calling app_main()[0m

[2657] [[32minfo[0m] [Main] === Cardputer ADV AI Assistant v1.0 ===

[2658] [[32minfo[0m] [HalFacade] creating hal facade instance

[2658] [[32minfo[0m] [HalFacade] hal facade init

[0;32mI (530) M5GFX: [Autodetect] board_M5CardputerADV[0m

[3232] [[32minfo[0m] [SystemUtils] system utils init

[3242] [[32minfo[0m] [DisplayManager] Canvas reset complete: valid=%d

[0;32mI (1092) Settings: SD settings not found: /sdcard/system/system.json[0m

[3243] [[32minfo[0m] [Input] input init

[3243] [[32minfo[0m] [Keyboard] init - FIX V10 (Global Lock)

[3246] [[32minfo[0m] [Power] power init

[3246] [[32minfo[0m] [Network] network init

[3247] [[32minfo[0m] [Storage] storage init

[3247] [[32minfo[0m] [LifeGuard] GuardData initialized (Safe-Heap)

[3247] [[32minfo[0m] [System] system init

[3247] [[32minfo[0m] [UIRenderer] ui renderer init

[3248] [[32minfo[0m] [HalFacade] all modules initialized

[3248] [[32minfo[0m] [ConfigStore] Config loaded: provider=deepseek model=deepseek-chat

[3249] [[32minfo[0m] [ModelRegistry] Registered 20 models

[3249] [[32minfo[0m] [AiEngine] init: ready=false

[3250] [[32minfo[0m] [ChatEngine] init

[3250] [[32minfo[0m] [ChatEngine] New conversation: 00000003f42d

[3250] [[32minfo[0m] [UiEngine] init

[3264] [[32minfo[0m] [SdCard] Initializing SD card...

[0;31mE (1316) sdmmc_sd: sdmmc_init_sd_if_cond: send_if_cond (1) returned 0x108[0m

[0;31mE (1316) vfs_fat_sdmmc: sdmmc_card_init failed (0x108).[0m

[3467] [[31merror[0m] [SdCard] SD mount failed: 264

[3468] [[32minfo[0m] [ToolMgr] init

[3468] [[32minfo[0m] [MCP] init

[3468] [[32minfo[0m] [Main] First boot - setup mode

[3468] [[32minfo[0m] [Main] System init done, state=1



assert failed: tcpip_send_msg_wait_sem /IDF/components/lwip/lwip/src/api/tcpip.c:454 (Invalid mbox)





Backtrace: 0x40381489:0x3fcb26b0 0x40381451:0x3fcb26d0 0x40389a39:0x3fcb26f0 0x4208ccd3:0x3fcb2810 0x4209ed66:0x3fcb2840 0x4209edfa:0x3fcb2860 0x4208c321:0x3fcb28b0 0x4204e8ea:0x3fcb28d0 0x4204efe0:0x3fcb2930 0x420143e5:0x3fcb2960 0x4200af55:0x3fcb2a10 0x4200b978:0x3fcb2a90 0x40382199:0x3fcb2ab0









ELF file SHA256: a7a40d7db



Rebooting...

ESP-ROM:esp32s3-20210327

Build:Mar 27 2021

rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)

Saved PC:0x40381608

SPIWP:0xee

mode:DIO, clock div:1

load:0x3fce2820,len:0x1700

load:0x403c8700,len:0xe1c

load:0x403cb700,len:0x31cc

entry 0x403c8938

I (24) boot: ESP-IDF v5.5.1-dirty 2nd stage bootloader

I (24) boot: compile time Apr 28 2026 00:52:34

I (24) boot: Multicore bootloader

I (25) boot: chip revision: v0.2

I (25) boot: efuse block revision: v1.3

I (26) qio_mode: Enabling default flash chip QIO

I (26) boot.esp32s3: Boot SPI Speed : 80MHz

I (26) boot.esp32s3: SPI Mode       : QIO

I (26) boot.esp32s3: SPI Flash Size : 8MB

I (27) boot: Enabling RNG early entropy source...

I (27) boot: Partition Table:

I (27) boot: ## Label            Usage          Type ST Offset   Length

I (28) boot:  0 nvs              WiFi data        01 02 00009000 00004000

I (28) boot:  1 otadata          OTA data         01 00 0000d000 00002000

I (29) boot:  2 phy_init         RF data          01 01 0000f000 00001000

I (30) boot:  3 factory          factory app      00 00 00010000 00400000

I (30) boot:  4 storage          Unknown data     01 82 00410000 00380000

I (31) boot: End of partition table

I (31) boot: Defaulting to factory image

I (32) esp_image: segment 0: paddr=00010020 vaddr=3c120020 size=1af6f4h (1767156) map

