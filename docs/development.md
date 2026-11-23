# Firmware development

Start with the [build and setup steps](../README.md#build-and-run) using ESP-IDF 6.1, ESP32-S3, and 16 MB flash. The [step-by-step Chinese guide](development.zh-CN.md) includes complete source for a small, read-only extension and its verification steps.

| Change | Entry point |
| --- | --- |
| Startup sequence and default port | [app_main()](../firmware/main/main.c) |
| Pin assignments and electrical logic | [DI BSP](../firmware/main/BSP/DI/app_DI.c), [DO BSP](../firmware/main/BSP/DO/app_DO.c), [GPIO mapping](../hardware/README.md#gpio-mapping) |
| Modbus device templates | [RS485 engine](../firmware/main/Logic/RS485Engine/app_rs485_engine.c), [point definitions](../firmware/main/Logic/RS485Engine/include/app_rs485_engine.h) |
| CAN frame handling | [CAN engine](../firmware/main/Logic/CANEngine/app_can_engine.c), [TWAI driver](../firmware/main/BSP/CAN/app_can.c) |
| Browser UI, authentication and API routes | [Web server](../firmware/main/Logic/WebServer/app_webserver.c) |
| Source files and include paths | [Component CMakeLists.txt](../firmware/main/CMakeLists.txt) |

## First extension

The example adds `GET /api/di/summary` to count active inputs. Put the counting logic in its own module under `firmware/main/Logic` and call `bsp_di_get_raw()` for channels 1–4. The driver already converts active-low optocoupler signals into logical input state. Add the source and header directory to `SRCS` and `INCLUDE_DIRS`. Call the module from a handler protected by `CHECK_AUTH`, and register the new GET route next to `/api/di`.

The [complete example](development.zh-CN.md#练习新增只读-di-汇总接口) was built in an isolated copy. On a test device running the example, sign in and compare `active` with the sum of `di1` through `di4` from the existing endpoint while the inputs are steady. After sign-out, the new route should return HTTP 401. These device-side checks are pending.

For new Modbus devices, bring up a single custom point using the device register map before adding a predefined template. Applying a template replaces the configured points. For new CAN messages, extend the CAN engine and compare captured frames on a bench network before testing an output. Keep GPIO changes aligned with the [schematics](../hardware/schematics/README.md).

See the [ESP-IDF HTTP Server reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/protocols/esp_http_server.html).
