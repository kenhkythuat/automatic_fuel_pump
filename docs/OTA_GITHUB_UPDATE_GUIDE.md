# Hướng dẫn phát hành OTA ESP32-S3 qua GitHub

## 1. Mục đích

Tài liệu này mô tả các bước chuẩn bị, build và đưa firmware lên GitHub để thiết bị ESP32-S3 tự phát hiện phiên bản mới và OTA.

Hướng dẫn này áp dụng cho cấu hình hiện tại của project:

| Thành phần | Giá trị hiện tại |
|---|---|
| Repository | `kenhkythuat/automatic_fuel_pump` |
| Git remote | `origin_2` |
| Nhánh OTA | `read_rs232_printer` |
| Target | `esp32s3` |
| Chu kỳ kiểm tra | 30 giây |
| Manifest | `releases/esp32s3/version.json` |
| Firmware OTA | `releases/esp32s3/atc_wifi_fw.bin` |
| URL manifest | `https://raw.githubusercontent.com/kenhkythuat/automatic_fuel_pump/read_rs232_printer/releases/esp32s3/version.json` |

## 2. Cơ chế OTA hiện tại

Thiết bị thực hiện các bước sau:

1. Kết nối Wi-Fi và khởi tạo chương trình chính.
2. Task OTA tải `version.json` từ GitHub Raw mỗi 30 giây.
3. Đọc trường số `build` trong manifest.
4. So sánh `remote build` với `fwVerion` đang lưu trong NVS.
5. Chỉ OTA khi:

   ```text
   remote build > current build
   ```

6. Tải file tại `firmware_url` vào partition OTA kế tiếp.
7. Nếu tải và xác thực image thành công:
   - Lưu `build` mới vào NVS key `fwVerion`.
   - Đặt `OperationMode = 1`.
   - Reboot.
8. Sau khi boot firmware mới, ứng dụng đánh dấu image hợp lệ và hủy rollback.

Các trường trong `version.json` hiện được sử dụng như sau:

| Trường | Trạng thái |
|---|---|
| `build` | Dùng để quyết định có OTA hay không; bắt buộc nằm trong `0..255` |
| `target` | Nếu có, phải bằng `esp32s3` |
| `firmware_url` | URL tải firmware; nếu thiếu sẽ dùng URL mặc định trong code |
| `version` | Chỉ dùng để hiển thị log, không quyết định OTA |
| `force` | Chưa được code xử lý |
| `sha256` | Chưa được code kiểm tra |

> Quan trọng: tăng chuỗi `version` nhưng không tăng `build` sẽ không làm thiết bị OTA.

## 3. Các file cần đồng bộ phiên bản

Trước khi build một bản phát hành mới, nên đồng bộ cùng một số build tại:

1. `main/main.cpp`

   ```cpp
   #define DEFAULT_FW_VERSION 19
   ```

2. `nvs.csv`, dành cho thiết bị nạp mới tại nhà máy:

   ```csv
   fwVerion,data,u8,19
   ```

3. `releases/esp32s3/version.json`:

   ```json
   {
     "version": "1.0.19",
     "build": 19,
     "target": "esp32s3",
     "force": false,
     "firmware_url": "https://raw.githubusercontent.com/kenhkythuat/automatic_fuel_pump/read_rs232_printer/releases/esp32s3/atc_wifi_fw.bin",
     "sha256": ""
   }
   ```

`nvs.csv` không nằm trong file OTA ứng dụng. OTA chỉ tải `atc_wifi_fw.bin`, vì vậy ID thiết bị, Wi-Fi, giá tiền và các cài đặt đang lưu trong NVS sẽ được giữ lại.

## 4. Quy trình phát hành nhanh

### Bước 1: Kiểm tra đúng nhánh và remote

```powershell
git branch --show-current
git remote -v
```

Kết quả mong đợi:

```text
read_rs232_printer
origin_2  git@github.com:kenhkythuat/automatic_fuel_pump.git
```

Nếu cần cập nhật code mới nhất:

```powershell
git pull origin_2 read_rs232_printer
```

Không pull khi đang có thay đổi chưa commit nếu chưa kiểm tra khả năng xung đột.

### Bước 2: Chọn số build mới

- Số build mới phải lớn hơn build đang chạy trên thiết bị.
- Số build tối đa hiện tại là `255` vì firmware dùng `uint8_t` và NVS `u8`.
- Không tái sử dụng một số build đã phát hành.

Ví dụ:

```text
Thiết bị đang chạy build 18
Phiên bản mới phải dùng build 19 hoặc lớn hơn
```

Cập nhật `DEFAULT_FW_VERSION` trong `main/main.cpp` và `fwVerion` trong `nvs.csv` trước khi build.

### Bước 3: Build firmware

Mở ESP-IDF PowerShell hoặc terminal đã chạy `export.ps1`, sau đó:

```powershell
idf.py fullclean
idf.py build
```

Không cần chạy `idf.py set-target esp32s3` cho mỗi lần phát hành nếu project đã có đúng `sdkconfig`. Chỉ dùng lại lệnh này khi thực sự đổi target hoặc tạo cấu hình mới.

Build thành công phải có dòng tương tự:

```text
Project build complete
Generated .../build/atc_wifi_fw.bin
```

Kiểm tra app không vượt partition nhỏ nhất `0x280000` byte:

```text
atc_wifi_fw.bin binary size ...
Smallest app partition is 0x280000 bytes
```

### Bước 4: Chép firmware sang thư mục phát hành

```powershell
Copy-Item -Force build\atc_wifi_fw.bin releases\esp32s3\atc_wifi_fw.bin
```

Kiểm tra file build và file phát hành giống nhau:

```powershell
Get-FileHash build\atc_wifi_fw.bin -Algorithm SHA256
Get-FileHash releases\esp32s3\atc_wifi_fw.bin -Algorithm SHA256
```

Hai giá trị hash phải giống hệt nhau.

### Bước 5: Cập nhật `version.json`

Cập nhật tối thiểu:

- `version`: tên phiên bản dễ đọc.
- `build`: số nguyên mới, lớn hơn phiên bản trên thiết bị.
- `target`: giữ nguyên `esp32s3`.
- `firmware_url`: phải trỏ đúng repository, nhánh và file binary.

Kiểm tra JSON hợp lệ và không có dấu phẩy thừa.

### Bước 6: Kiểm tra thay đổi trước khi commit

```powershell
git status --short
git diff -- main/main.cpp nvs.csv releases/esp32s3/version.json
```

Đảm bảo `releases/esp32s3/atc_wifi_fw.bin` đã thay đổi. Không commit file tạm của Word như `~$*.docx`.

### Bước 7: Commit và push

Chỉ add các file thuộc bản phát hành:

```powershell
git add main/main.cpp nvs.csv releases/esp32s3/version.json releases/esp32s3/atc_wifi_fw.bin
git add <các-file-source-đã-sửa>
git commit -m "release: esp32s3 firmware build 19"
git push origin_2 read_rs232_printer
```

Không dùng `git add .` nếu workspace có file cấu hình thiết bị, file tạm hoặc thay đổi chưa muốn phát hành.

## 5. Quy trình khuyến nghị để tránh binary cũ do cache

GitHub Raw có thể trả manifest hoặc binary cũ trong một khoảng thời gian. Cách an toàn nhất là để `firmware_url` trỏ tới commit chứa binary thay vì trỏ trực tiếp tới tên nhánh.

### Commit 1: source và binary

1. Sửa code và số phiên bản mặc định.
2. Build.
3. Chép binary vào `releases/esp32s3/atc_wifi_fw.bin`.
4. Chưa tăng `build` trong `version.json`.
5. Commit source và binary:

   ```powershell
   git add main nvs.csv releases/esp32s3/atc_wifi_fw.bin
   git commit -m "build: prepare esp32s3 firmware 19"
   ```

6. Lấy commit SHA chứa binary:

   ```powershell
   git rev-parse HEAD
   ```

Giả sử SHA nhận được là `<FIRMWARE_COMMIT_SHA>`.

### Commit 2: công bố manifest

Cập nhật `version.json` sau cùng:

```json
{
  "version": "1.0.19",
  "build": 19,
  "target": "esp32s3",
  "force": false,
  "firmware_url": "https://raw.githubusercontent.com/kenhkythuat/automatic_fuel_pump/<FIRMWARE_COMMIT_SHA>/releases/esp32s3/atc_wifi_fw.bin",
  "sha256": ""
}
```

Sau đó:

```powershell
git add releases/esp32s3/version.json
git commit -m "release: publish esp32s3 firmware 1.0.19"
git push origin_2 read_rs232_printer
```

Có thể tạo cả hai commit ở máy rồi push một lần. Khi thiết bị nhìn thấy manifest mới, URL binary đã bất biến và không thể trỏ nhầm sang binary của lần phát hành khác.

## 6. Kiểm tra trên GitHub trước khi chờ thiết bị OTA

Mở hoặc tải trực tiếp hai URL:

```text
https://raw.githubusercontent.com/kenhkythuat/automatic_fuel_pump/read_rs232_printer/releases/esp32s3/version.json
https://raw.githubusercontent.com/kenhkythuat/automatic_fuel_pump/read_rs232_printer/releases/esp32s3/atc_wifi_fw.bin
```

Kiểm tra manifest không dùng cache bằng PowerShell:

```powershell
$url = "https://raw.githubusercontent.com/kenhkythuat/automatic_fuel_pump/read_rs232_printer/releases/esp32s3/version.json?ts=$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())"
(Invoke-WebRequest -UseBasicParsing $url).Content
```

Tham số `?ts=...` chỉ phục vụ kiểm tra thủ công. Firmware hiện vẫn tải URL cố định trong `main/ota_update.c`, nên nếu log còn thấy build cũ hãy chờ các chu kỳ 30 giây tiếp theo và kiểm tra lại GitHub Raw.

Nếu dùng URL binary theo commit SHA như mục 5, có thể tải binary về và so sánh hash với file local:

```powershell
Invoke-WebRequest -UseBasicParsing "<FIRMWARE_URL>" -OutFile "$env:TEMP\atc_wifi_fw_remote.bin"
Get-FileHash releases\esp32s3\atc_wifi_fw.bin -Algorithm SHA256
Get-FileHash "$env:TEMP\atc_wifi_fw_remote.bin" -Algorithm SHA256
```

## 7. Log OTA mong đợi

Thiết bị bắt đầu kiểm tra:

```text
OTA: GitHub OTA version check started, interval=30000 ms, url=...
```

Phát hiện bản mới:

```text
OTA: GitHub version.json: version=1.0.19 build=19 url=...
OTA: New firmware available: current=18 remote=19
OTA: Starting OTA from URL: .../atc_wifi_fw.bin
```

Hoàn tất:

```text
OTA: OTA successful. New build=19. Rebooting...
OTA: Running OTA app marked valid, rollback cancelled
MAIN: Fw version 19
```

Nếu không có bản mới:

```text
OTA: Firmware is up to date: current=19 remote=19
```

## 8. Xử lý lỗi thường gặp

| Log hoặc hiện tượng | Nguyên nhân thường gặp | Cách xử lý |
|---|---|---|
| `Firmware is up to date: current=18 remote=18` dù đã sửa local | Chưa push manifest, push sai remote/nhánh hoặc GitHub Raw còn cache | Kiểm tra `origin_2`, nhánh `read_rs232_printer`, URL Raw có cache-busting và commit trên GitHub |
| `File not found (404)` | `firmware_url` sai nhánh, sai tên file hoặc file chưa được push | Mở URL trực tiếp; kiểm tra đúng `releases/esp32s3/atc_wifi_fw.bin` |
| `target mismatch` | `target` không phải `esp32s3` | Sửa trường `target` |
| `missing valid build 0..255` | Thiếu `build`, sai kiểu hoặc vượt 255 | Dùng số nguyên trong khoảng `0..255` |
| Thiết bị không OTA dù `version` mới | `build` chưa tăng | Tăng `build` lớn hơn `fwVerion` hiện tại |
| OTA tải thành công nhưng firmware không đổi như mong muốn | Binary trong `releases` chưa được chép lại sau build | So sánh SHA256 của `build` và `releases` |
| `ESP HTTPS OTA begin failed` | Mạng, DNS, TLS, timeout hoặc URL lỗi | Kiểm tra Wi-Fi, Internet, HTTP status và URL GitHub Raw |
| `OTA image validation failed` | Binary hỏng hoặc không đúng target | Build lại với ESP32-S3, chép lại và so sánh hash |
| App partition quá nhỏ | Binary lớn hơn `0x280000` | Giảm kích thước firmware hoặc thiết kế lại partition trước khi phát hành |

## 9. Checklist trước khi phát hành

- [ ] Đang ở nhánh `read_rs232_printer`.
- [ ] Remote push là `origin_2`.
- [ ] Code đã được kiểm thử.
- [ ] `DEFAULT_FW_VERSION` đã cập nhật.
- [ ] `nvs.csv` đã đồng bộ cho thiết bị nạp mới.
- [ ] `build` mới lớn hơn phiên bản đang chạy và không vượt 255.
- [ ] `idf.py build` thành công.
- [ ] Binary nhỏ hơn partition `0x280000`.
- [ ] Đã chép `build/atc_wifi_fw.bin` sang `releases/esp32s3/`.
- [ ] SHA256 của hai file local giống nhau.
- [ ] `version.json` hợp lệ và có đúng `target`/`firmware_url`.
- [ ] Source, binary và manifest đã được commit.
- [ ] Đã push lên `origin_2/read_rs232_printer`.
- [ ] Hai URL GitHub Raw trả HTTP 200.
- [ ] Đã theo dõi log thiết bị qua ít nhất vài chu kỳ 30 giây.

## 10. Các giới hạn cần nhớ

- `build` hiện chỉ có phạm vi `0..255`. Trước khi vượt 255 cần đổi `u8FwVerion`, NVS encoding và parser sang kiểu lớn hơn.
- `sha256` trong manifest chưa được firmware kiểm tra.
- `force` chưa có tác dụng.
- Firmware hiện đánh dấu app OTA hợp lệ khá sớm trong `app_main`; chưa có health-check dài hạn trước khi hủy rollback.
- GitHub repository phải cho phép thiết bị tải các URL Raw mà không cần đăng nhập.
- OTA chỉ cập nhật app partition, không cập nhật NVS factory hoặc partition table.
