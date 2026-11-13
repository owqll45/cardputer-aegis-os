22:19:57 [👤 用户] 屏幕已清空
22:19:57 [👤 用户] 正在连接 COM9...
22:19:57 [🤖 系统] 连接成功！等待数据...
[16241] [[32minfo[0m] [2048] Application opened

[16255] [[31merror[0m] [2048] Failed to create sprite, fallback

[21773] [[32minfo[0m] [2048] Application opened

[21788] [[31merror[0m] [2048] Failed to create sprite, fallback

[25852] [[32minfo[0m] [Snake] Application opened

[25866] [[31merror[0m] [Snake] Failed to create sprite, fallback

Guru Meditation Error: Core  0 panic'ed (IntegerDivideByZero). Exception was unhandled.



Core  0 register dump:

PC      : 0x4202ccec  PS      : 0x00060a30  A0      : 0x8202d425  A1      : 0x3fcb3d70  

A2      : 0x3fccd13c  A3      : 0x00000000  A4      : 0x00000000  A5      : 0x00000000  

A6      : 0x00000001  A7      : 0x4038364c  A8      : 0x00000000  A9      : 0x3fcb3d50  

A10     : 0x354dc21b  A11     : 0x000043dc  A12     : 0xd5400000  A13     : 0x42450000  

A14     : 0x3fcb3d18  A15     : 0x3c156568  SAR     : 0x0000000a  EXCCAUSE: 0x00000006  

EXCVADDR: 0x00000000  LBEG    : 0x400570e8  LEND    : 0x400570f3  LCOUNT  : 0x00000000  





Backtrace: 0x4202cce9:0x3fcb3d70 0x4202d422:0x3fcb3db0 0x4202d84e:0x3fcb3df0 0x42141e41:0x3fcb3e60 0x4205ad86:0x3fcb3e80 0x4205ade6:0x3fcb3ea0 0x4205ae29:0x3fcb3ed0 0x4200e7dc:0x3fcb3ef0 0x4214d8ab:0x3fcb4060 0x403847ea:0x3fcb4090









ELF file SHA256: ab6db3e1a



Rebooting...

ESP-ROM:esp32s3-20210327

Build:Mar 27 2021

rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)

Saved PC:0x40379c84

SPIWP:0xee

mode:DIO, clock div:1

load:0x3fce2810,len:0x148c

load:0x403c8700,len:0x4

load:0x403c8704,len:0xc10

load:0x403cb700,len:0x2dc0

entry 0x403c8904

I (26) boot: ESP-IDF v5.3.1-dirty 2nd stage bootloader

I (26) boot: compile time Jan 26 2026 00:47:21

I (26) boot: Multicore bootloader

I (27) boot: chip revision: v0.2

I (27) boot.esp32s3: Boot SPI Speed : 80MHz

I (27) boot.esp32s3: SPI Mode       : DIO

I (27) boot.esp32s3: SPI Flash Size : 8MB

I (27) boot: Enabling RNG early entropy source...

I (28) boot: Partition Table:

I (28) boot: ## Label            Usage          Type ST Offset   Length

I (28) boot:  0 nvs              WiFi data        01 02 00009000 00004000

I (28) boot:  1 otadata          OTA data         01 00 0000d000 00002000

I (29) boot:  2 phy_init         RF data          01 01 0000f000 00001000

I (29) boot:  3 factory          factory app      00 00 00010000 00400000

I (30) boot: End of partition table

I (30) boot: Defaulting to factory image

I (30) esp_image: segment 0: paddr=00010020 vaddr=3c150020 size=12b22ch (1225260) map

I (250) esp_image: segment 1: paddr=0013b254 vaddr=3fca1a00 size=04dc4h ( 19908) load

I (255) esp_image: segment 2: paddr=00140020 vaddr=42000020 size=14eba0h (1371040) map

I (501) esp_image: segment 3: paddr=0028ebc8 vaddr=3fca67c4 size=00778h (  1912) load

I (502) esp_image: segment 4: paddr=0028f348 vaddr=40378000 size=19944h (104772) load

I (525) esp_image: segment 5: paddr=002a8c94 vaddr=600fe000 size=00108h (   264) load

I (536) boot: Loaded app from partition at offset 0x10000

I (536) boot: Disabling RNG early entropy source...

I (537) cpu_start: Multicore app

I (546) cpu_start: Pro cpu start user code

I (546) cpu_start: cpu freq: 240000000 Hz

I (546) app_init: Application information:

I (547) app_init: Project name:     cardputer-adv

I (547) app_init: App version:      1

I (547) app_init: Compile time:     Jan 26 2026 00:46:52

I (547) app_init: ELF file SHA256:  ab6db3e1a...

I (548) app_init: ESP-IDF:          v5.3.1-dirty

I (548) efuse_init: Min chip rev:     v0.0

I (548) efuse_init: Max chip rev:     v0.99 

I (548) efuse_init: Chip rev:         v0.2

I (548) heap_init: Initializing. RAM available for dynamic allocation:

I (549) heap_init: At 3FCB0348 len 000393C8 (228 KiB): RAM

I (549) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM

I (549) heap_init: At 600FE108 len 00001EE0 (7 KiB): RTCRAM

I (550) spi_flash: detected chip: gd

I (550) spi_flash: flash io: dio

I (552) sleep: Configure to isolate all GPIO pins in sleep state

I (552) sleep: Enable automatic switching of GPIO sleep configuration

I (553) coexist: coex firmware version: 4482466

I (553) coexist: coexist rom version e7ae62f

I (553) main_task: Started on CPU0

I (554) main_task: Calling app_main()

[26389] [[32minfo[0m] [HAL] creating hal instance

[26390] [[32minfo[0m] [HAL] init

I (572) gpio: GPIO[5]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (573) gpio: GPIO[6]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (573) gpio: GPIO[4]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (573) gpio: GPIO[5]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (574) gpio: GPIO[6]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (577) gpio: GPIO[21]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (577) gpio: GPIO[17]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (577) gpio: GPIO[33]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (578) gpio: GPIO[21]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (578) gpio: GPIO[17]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (580) gpio: GPIO[5]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (580) gpio: GPIO[6]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (580) gpio: GPIO[4]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (581) gpio: GPIO[5]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (581) gpio: GPIO[6]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (583) gpio: GPIO[35]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (583) gpio: GPIO[36]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (583) gpio: GPIO[34]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (584) gpio: GPIO[35]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (584) gpio: GPIO[36]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (585) M5GFX: [Autodetect] board_M5CardputerADV

[26980] [[32minfo[0m] [HAL] display init

[26981] [[32minfo[0m] [HAL] i2c scan

     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f


00: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 


10: -- -- -- -- -- -- -- -- 18 -- -- -- -- -- -- -- 


20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 


30: -- -- -- -- 34 -- -- -- -- -- -- -- -- -- -- -- 


40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 


50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 


60: -- -- -- -- -- -- -- -- -- 69 -- -- -- -- -- -- 


70: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 


[27001] [[32minfo[0m] [HAL] keyboard init

[27002] [[32minfo[0m] [Keyboard] init

I (1169) gpio: GPIO[11]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:3 

[27005] [[32minfo[0m] [HAL] setting init

[27024] [[32minfo[0m] [HAL] spi init

[27025] [[32minfo[0m] [HAL] spi bus initialized



 _____ _____ _____ _____ _____ _____ _____ _____

|     |     |     |   | |     |  _  |  |  |   __|

| | | |  |  |  |  | | | |   --|     |    -|   __|

|_|_|_|_____|_____|_|___|_____|__|__|__|__|_____|



- @author Forairaaaaa

- @version 2.0.0

- @build at 00:48:11 Jan 26 2026



[27028] [[32minfo[0m] [System] Starting System WiFi Daemon...

[27028] [[32minfo[0m] [HAL] wifi init

I (1195) pp: pp rom version: e7ae62f

I (1196) net80211: net80211 rom version: e7ae62f

I (1197) wifi:wifi driver task: 3fcd3928, prio:23, stack:6656, core=0

I (1206) wifi:wifi firmware version: ccaebfa

I (1206) wifi:wifi certification version: v7.0

I (1206) wifi:config NVS flash: enabled

I (1206) wifi:config nano formating: disabled

I (1206) wifi:Init data frame dynamic rx buffer num: 32

I (1207) wifi:Init static rx mgmt buffer num: 5

I (1207) wifi:Init management short buffer num: 32

I (1207) wifi:Init dynamic tx buffer num: 32

I (1207) wifi:Init static tx FG buffer num: 2

I (1207) wifi:Init static rx buffer size: 1600

I (1207) wifi:Init static rx buffer num: 10

I (1208) wifi:Init dynamic rx buffer num: 32

I (1208) wifi_init: rx ba win: 6

I (1209) wifi_init: accept mbox: 6

I (1209) wifi_init: tcpip mbox: 32

I (1209) wifi_init: udp mbox: 6

I (1209) wifi_init: tcp mbox: 6

I (1209) wifi_init: tcp tx win: 5760

I (1209) wifi_init: tcp rx win: 5760

I (1209) wifi_init: tcp mss: 1440

I (1209) wifi_init: WiFi IRAM OP enabled

I (1209) wifi_init: WiFi RX IRAM OP enabled

I (1211) phy_init: phy_version 680,a6008b2,Jun  4 2024,16:41:10

W (1243) phy_init: saving new calibration data because of checksum failure, mode(0)

I (1257) wifi:mode : sta (d8:85:ac:a5:af:00)

I (1258) wifi:enable tsf

I (1260) wifi:flush txq

I (1261) wifi:stop sw txq

I (1261) wifi:lmac stop hw txq

I (1266) wifi:mode : softAP (d8:85:ac:a5:af:01)

I (1269) wifi:Total power save buffer number: 16

I (1269) wifi:Init max length of beacon: 752/752

I (1270) wifi:Init max length of beacon: 752/752

I (1271) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 192.168.4.1

[27109] [[32minfo[0m] [audio] set keyboard sfx enable: true

[27111] [[32minfo[0m] [Launcher] on create

[27111] [[32minfo[0m] [Launcher] start boot anim

[27111] [[32minfo[0m] [Launcher] not power on reset, skip boot anim

[27112] [[32minfo[0m] [LauncherMenu] init

[33171] [[32minfo[0m] [Snake] Application opened

[33185] [[31merror[0m] [Snake] Failed to create sprite, fallback

Guru Meditation Error: Core  0 panic'ed (IntegerDivideByZero). Exception was unhandled.



Core  0 register dump:

PC      : 0x4202ccec  PS      : 0x00060230  A0      : 0x8202d425  A1      : 0x3fcb3d70  

A2      : 0x3fccd13c  A3      : 0x00000000  A4      : 0x00000000  A5      : 0x00000000  

A6      : 0x00000001  A7      : 0x4038364c  A8      : 0x00000000  A9      : 0x3fcb3d50  

A10     : 0x5c6f7a13  A11     : 0x00001a92  A12     : 0x13c00000  A13     : 0x19f20000  

A14     : 0x3fcb3d18  A15     : 0x3c156568  SAR     : 0x0000000a  EXCCAUSE: 0x00000006  

EXCVADDR: 0x00000000  LBEG    : 0x400570e8  LEND    : 0x400570f3  LCOUNT  : 0x00000000  





Backtrace: 0x4202cce9:0x3fcb3d70 0x4202d422:0x3fcb3db0 0x4202d84e:0x3fcb3df0 0x42141e41:0x3fcb3e60 0x4205ad86:0x3fcb3e80 0x4205ade6:0x3fcb3ea0 0x4205ae29:0x3fcb3ed0 0x4200e7dc:0x3fcb3ef0 0x4214d8ab:0x3fcb4060 0x403847ea:0x3fcb4090









ELF file SHA256: ab6db3e1a



Rebooting...

ESP-ROM:esp32s3-20210327

Build:Mar 27 2021

rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)

Saved PC:0x40379c84

SPIWP:0xee

mode:DIO, clock div:1

load:0x3fce2810,len:0x148c

load:0x403c8700,len:0x4

load:0x403c8704,len:0xc10

load:0x403cb700,len:0x2dc0

entry 0x403c8904

I (26) boot: ESP-IDF v5.3.1-dirty 2nd stage bootloader

I (26) boot: compile time Jan 26 2026 00:47:21

I (26) boot: Multicore bootloader

I (27) boot: chip revision: v0.2

I (27) boot.esp32s3: Boot SPI Speed : 80MHz

I (27) boot.esp32s3: SPI Mode       : DIO

I (27) boot.esp32s3: SPI Flash Size : 8MB

I (28) boot: Enabling RNG early entropy source...

I (28) boot: Partition Table:

I (28) boot: ## Label            Usage          Type ST Offset   Length

I (28) boot:  0 nvs              WiFi data        01 02 00009000 00004000

I (29) boot:  1 otadata          OTA data         01 00 0000d000 00002000

I (29) boot:  2 phy_init         RF data          01 01 0000f000 00001000

I (29) boot:  3 factory          factory app      00 00 00010000 00400000

I (30) boot: End of partition table

I (30) boot: Defaulting to factory image

I (30) esp_image: segment 0: paddr=00010020 vaddr=3c150020 size=12b22ch (1225260) map

I (250) esp_image: segment 1: paddr=0013b254 vaddr=3fca1a00 size=04dc4h ( 19908) load

I (255) esp_image: segment 2: paddr=00140020 vaddr=42000020 size=14eba0h (1371040) map

I (501) esp_image: segment 3: paddr=0028ebc8 vaddr=3fca67c4 size=00778h (  1912) load

I (502) esp_image: segment 4: paddr=0028f348 vaddr=40378000 size=19944h (104772) load

I (525) esp_image: segment 5: paddr=002a8c94 vaddr=600fe000 size=00108h (   264) load

I (536) boot: Loaded app from partition at offset 0x10000

I (536) boot: Disabling RNG early entropy source...

I (537) cpu_start: Multicore app

I (546) cpu_start: Pro cpu start user code

I (546) cpu_start: cpu freq: 240000000 Hz

I (547) app_init: Application information:

I (547) app_init: Project name:     cardputer-adv

I (547) app_init: App version:      1

I (547) app_init: Compile time:     Jan 26 2026 00:46:52

I (547) app_init: ELF file SHA256:  ab6db3e1a...

I (547) app_init: ESP-IDF:          v5.3.1-dirty

I (548) efuse_init: Min chip rev:     v0.0

I (548) efuse_init: Max chip rev:     v0.99 

I (548) efuse_init: Chip rev:         v0.2

I (548) heap_init: Initializing. RAM available for dynamic allocation:

I (548) heap_init: At 3FCB0348 len 000393C8 (228 KiB): RAM

I (549) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM

I (549) heap_init: At 600FE108 len 00001EE0 (7 KiB): RTCRAM

I (550) spi_flash: detected chip: gd

I (550) spi_flash: flash io: dio

I (552) sleep: Configure to isolate all GPIO pins in sleep state

I (552) sleep: Enable automatic switching of GPIO sleep configuration

I (552) coexist: coex firmware version: 4482466

I (553) coexist: coexist rom version e7ae62f

I (553) main_task: Started on CPU0

I (554) main_task: Calling app_main()

[33734] [[32minfo[0m] [HAL] creating hal instance

[33735] [[32minfo[0m] [HAL] init

I (572) gpio: GPIO[5]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (573) gpio: GPIO[6]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (573) gpio: GPIO[4]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (573) gpio: GPIO[5]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (573) gpio: GPIO[6]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (576) gpio: GPIO[21]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (577) gpio: GPIO[17]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (577) gpio: GPIO[33]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (577) gpio: GPIO[21]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (578) gpio: GPIO[17]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (579) gpio: GPIO[5]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (580) gpio: GPIO[6]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (580) gpio: GPIO[4]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (580) gpio: GPIO[5]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (581) gpio: GPIO[6]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (582) gpio: GPIO[35]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (583) gpio: GPIO[36]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (583) gpio: GPIO[34]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (583) gpio: GPIO[35]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (584) gpio: GPIO[36]| InputEn: 0| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 

I (584) M5GFX: [Autodetect] board_M5CardputerADV

[34324] [[32minfo[0m] [HAL] display init

[34325] [[32minfo[0m] [HAL] i2c scan

     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f


00: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 


10: -- -- -- -- -- -- -- -- 18 -- -- -- -- -- -- -- 


20: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 


30: -- -- -- -- 34 -- -- -- -- -- -- -- -- -- -- -- 


40: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 


50: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 


60: -- -- -- -- -- -- -- -- -- 69 -- -- -- -- -- -- 


70: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 


[34345] [[32minfo[0m] [HAL] keyboard init

[34345] [[32minfo[0m] [Keyboard] init

I (1168) gpio: GPIO[11]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:3 

[34349] [[32minfo[0m] [HAL] setting init

[34367] [[32minfo[0m] [HAL] spi init

[34368] [[32minfo[0m] [HAL] spi bus initialized



 _____ _____ _____ _____ _____ _____ _____ _____

|     |     |     |   | |     |  _  |  |  |   __|

| | | |  |  |  |  | | | |   --|     |    -|   __|

|_|_|_|_____|_____|_|___|_____|__|__|__|__|_____|



- @author Forairaaaaa

- @version 2.0.0

- @build at 00:48:11 Jan 26 2026



[34372] [[32minfo[0m] [System] Starting System WiFi Daemon...

[34372] [[32minfo[0m] [HAL] wifi init

I (1195) pp: pp rom version: e7ae62f

I (1195) net80211: net80211 rom version: e7ae62f

I (1197) wifi:wifi driver task: 3fcd3928, prio:23, stack:6656, core=0

I (1206) wifi:wifi firmware version: ccaebfa

I (1206) wifi:wifi certification version: v7.0

I (1206) wifi:config NVS flash: enabled

I (1206) wifi:config nano formating: disabled

I (1207) wifi:Init data frame dynamic rx buffer num: 32

I (1207) wifi:Init static rx mgmt buffer num: 5

I (1207) wifi:Init management short buffer num: 32

I (1207) wifi:Init dynamic tx buffer num: 32

I (1207) wifi:Init static tx FG buffer num: 2

I (1208) wifi:Init static rx buffer size: 1600

I (1208) wifi:Init static rx buffer num: 10

I (1208) wifi:Init dynamic rx buffer num: 32

I (1209) wifi_init: rx ba win: 6

I (1209) wifi_init: accept mbox: 6

I (1209) wifi_init: tcpip mbox: 32

I (1209) wifi_init: udp mbox: 6

I (1209) wifi_init: tcp mbox: 6

I (1209) wifi_init: tcp tx win: 5760

I (1209) wifi_init: tcp rx win: 5760

I (1209) wifi_init: tcp mss: 1440

I (1210) wifi_init: WiFi IRAM OP enabled

I (1210) wifi_init: WiFi RX IRAM OP enabled

I (1211) phy_init: phy_version 680,a6008b2,Jun  4 2024,16:41:10

I (1243) wifi:mode : sta (d8:85:ac:a5:af:00)

I (1243) wifi:enable tsf

I (1246) wifi:flush txq

I (1246) wifi:stop sw txq

I (1246) wifi:lmac stop hw txq

I (1251) wifi:mode : softAP (d8:85:ac:a5:af:01)

I (1254) wifi:Total power save buffer number: 16

I (1255) wifi:Init max length of beacon: 752/752

I (1255) wifi:Init max length of beacon: 752/752

I (1257) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 192.168.4.1

[34439] [[32minfo[0m] [audio] set keyboard sfx enable: true

[34441] [[32minfo[0m] [Launcher] on create

[34441] [[32minfo[0m] [Launcher] start boot anim

[34442] [[32minfo[0m] [Launcher] not power on reset, skip boot anim

[34442] [[32minfo[0m] [LauncherMenu] init

E (8516) adc_oneshot: adc_oneshot_new_unit(96): adc1 is already in use

I (8517) gpio: GPIO[2]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 0| Pulldown: 1| Intr:0 

[47208] [[32minfo[0m] [BT Keyboard & Mouse] Application opened

[47209] [[32minfo[0m] [BT Keyboard & Mouse] Free Heap: %d

[47209] [[32minfo[0m] [BT Keyboard & Mouse] De-initializing WiFi for stability...

[47210] [[32minfo[0m] [HAL] wifi deinit

I (14031) wifi:flush txq

I (14032) wifi:stop sw txq

I (14032) wifi:lmac stop hw txq

I (14034) wifi:Deinit lldesc rx mblock:10

[47214] [[31merror[0m] [BT Keyboard & Mouse] Failed to create sprite

[47215] [[32minfo[0m] [BT Keyboard & Mouse] Initing BLE...

[53150] [[32minfo[0m] [FPS] Application opened

