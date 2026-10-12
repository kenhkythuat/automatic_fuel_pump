# Ngoại lệ và tình huống biên của giao dịch

## 1. Ma trận xử lý lỗi

| Tình huống | Cách phát hiện | Xử lý hiện tại | Log/response | Ảnh hưởng trạng thái |
|---|---|---|---|---|
| JSON không hợp lệ | `cJSON_Parse` thất bại | Bỏ command | Log lỗi MQTT JSON | Không tạo context |
| Thiếu `cmd` | Không có string `cmd` | ACK error | `missing cmd` | Không tạo context |
| Thiếu hoặc sai `msg_id` | Không phải 32 ký tự hex | ACK error không có msg_id hợp lệ | `missing_or_invalid_msg_id` | Không tạo context |
| Thiếu `param` | `param` không phải object | ACK error | `missing param` | Không tạo context |
| `qr_money` sai kiểu/range | Parse số thất bại hoặc vượt `uint32_t` | ACK error | `invalid qr_money` | Không tạo context |
| `qr_litter` sai kiểu/range | Parse thất bại hoặc lớn hơn `UINT32_MAX/10` | ACK error | `fail_config_litter` | Không tạo context |
| Hết heap khi copy tham số | `strdup` trả `NULL` | ACK error | `out of memory` hoặc `fail_config_litter` | Không tạo context |
| Không tạo được keypad task | `xTaskCreate != pdPASS` | ACK error | `task create failed` | Không tạo context |
| GPIO6 không từng ACTIVE | `active_payment_input_started=false` | Không cho RS232 claim giao dịch | `Payment completion ... ignored` | Context vẫn chờ/cancel |
| GPIO6 cạnh `0→1`, không có context | Không có active payment/msg_id | Không gửi input-switch event | `INPUT_SWITCH ACTIVE event ignored` | Không đổi payment state |
| GPIO6 đã là `1` từ trước | Đọc lại sau keypad done | Đánh dấu started nhưng không phát event cạnh | Log `already ACTIVE` | Giao dịch vẫn hợp lệ |
| RS232 thiếu một trường | Không parse đủ MGD, Thoi gian, money, liter, price | Chưa đặt receipt ready | Có raw ASCII nhưng không có `receipt parsed` | Chờ 7 giây rồi thử T7 |
| RS232 chưa đến sau GPIO6 IDLE | Chưa ready trong 7000 ms đầu | Reset collector, nhấn T7 và chờ thêm 7000 ms | Log `payment recovery T7` | Giữ nguyên context |
| T7 trả hóa đơn cũ | `Thoi gian == lastTxnTime` trong NVS | Không claim giao dịch | `Reject duplicate RS232 receipt` | Chờ hết cửa sổ T7 rồi báo missing |
| T7 không có hóa đơn mới | Không ready sau cửa sổ thứ hai | Gửi completed lỗi | `missing_rs232_receipt` | Context bị claim và xóa |
| RS232 đến sau event timeout | Context đã bị xóa | Receipt có thể parse nhưng không tạo completed | `no active transaction` | Không phục hồi giao dịch cũ |
| RS232 buffer đầy | Dài ≥ 2048 byte trước khi parse | Xóa phần hóa đơn chưa hoàn chỉnh | `RS232 receipt buffer full` | Có thể dẫn tới timeout |
| Queue receipt đầy | Queue độ dài 2 không nhận thêm được | Bỏ notification mới | `RS232 receipt notification queue is full` | Fallback GPIO6 vẫn có thể đọc receipt ready |
| MQTT mất kết nối lúc publish | `mqtt_connected=false` | Publish trả lỗi, không có retry riêng cho event | `MQTT not connected, skip topic=...` | Context có thể đã bị claim; event có nguy cơ mất |
| MQTT QoS 1 giao lặp | Broker/client redelivery | Device/backend có thể nhận lặp | Không có dedupe bền vững trên device | Backend phải dedupe bằng topic + msg_id + event |
| Command lặp cùng `msg_id` | Không có cache command đã xử lý | Có thể thay payment context hoặc tạo task mới | Log command accepted lần nữa | Cần backend tránh resend không kiểm soát |
| Command thanh toán mới khi context cũ active | Không có reject rõ ràng | Context mới có thể ghi đè context cũ | Không có ACK busy | Rủi ro ghép receipt sai giao dịch |
| Cancel đúng `msg_id` | So khớp context | Xóa context và nhấn `C` | ACK cancel `ok` | Không còn completed cho giao dịch |
| Cancel sai `msg_id` | Không khớp trong hàm clear | Context cũ không bị xóa nhưng ACK hiện vẫn `ok` | ACK có thể gây hiểu nhầm | Cần cải tiến nếu server cần xác nhận chặt |
| Hai thiết bị trùng `MGD` | MGD là bộ đếm cục bộ | Cho phép | Không phải lỗi parser | Dùng node_pay/msg_id để phân biệt |
| `ts` không đúng epoch | Thiết bị chưa đồng bộ clock | Vẫn gửi `time(NULL)` | Giá trị có thể nhỏ | Backend nên dùng receive time hoặc trạng thái sync |

## 2. `missing_rs232_receipt`

### Điều kiện

Lỗi chỉ được tạo khi:

```text
active payment tồn tại
AND payment đã từng ACTIVE
AND GPIO6 chuyển về 0
AND không parse được hóa đơn mới trong 7000 ms đầu
AND đã thử payment recovery T7
AND không parse được hóa đơn có Thoi gian mới trong 7000 ms tiếp theo
```

### Nguyên nhân khả dĩ

- Trụ bơm không xuất hóa đơn.
- Dữ liệu đến sau 7 giây.
- RX GPIO11, baudrate, GND hoặc mạch chuyển mức có vấn đề.
- Chuỗi thay đổi tên trường hoặc kiểu dấu phân cách.
- Hóa đơn thiếu `MGD`, `Thoi gian`, `Thanh tien`, `So lit` hoặc `Don gia`.
- Buffer bị tràn và reset.

### Cách debug

Phân loại theo log:

```text
Không có [RS232 RX ASCII]
→ không có dữ liệu vật lý vào UART.

Có [RS232 RX ASCII] nhưng không có RS232 receipt parsed
→ dữ liệu không thỏa parser.

RS232 receipt parsed xuất hiện sau missing_rs232_receipt
→ cả cửa sổ ban đầu và cửa sổ T7 đã hết trước khi dữ liệu hoàn chỉnh.
```

## 3. Race condition đã được kiểm soát

Hai đường có thể phát kết quả:

1. `payment_receipt_task` khi RS232 vừa parse xong;
2. fallback trong `payment_input_switch_update` sau GPIO6 về `0`.

Cả hai đều gọi `claim_active_payment_context()`. Context được claim và xóa trong cùng critical section nên chỉ đường đầu tiên thắng; đường còn lại thấy không còn active transaction và không publish lần hai.

## 4. Hạn chế hiện tại cần cân nhắc khi phát triển

### 4.1 Không lưu hàng đợi event khi MQTT offline

Nếu publish thất bại sau khi context đã bị claim, firmware không lưu event vào NVS để gửi lại. Đây là rủi ro mất kết quả giao dịch.

Khuyến nghị tương lai:

- lưu outbox bền vững trong NVS hoặc partition riêng;
- chỉ đánh dấu event delivered sau MQTT PUBACK;
- retry với cùng event ID để backend dedupe.

### 4.2 Command lặp và command chồng nhau

Firmware chưa có bảng `msg_id` đã xử lý và chưa trả lỗi `busy` khi một payment context khác đang active.

Khuyến nghị:

- reject command mới bằng ACK `busy` khi đang có giao dịch;
- hoặc chỉ trả lại ACK cũ nếu `msg_id` bị gửi lặp;
- không ghi đè context đang hoạt động.

### 4.3 Cancel chưa xác nhận kết quả vật lý

ACK cancel được gửi sau khi tạo task, trước khi chứng minh phím `C` đã được thiết bị ngoài nhận. Cancel sai `msg_id` hiện vẫn có khả năng nhận ACK `ok` dù context không bị xóa.

Khuyến nghị:

- trả `not_found` hoặc `msg_id_mismatch` nếu context không khớp;
- cân nhắc event riêng sau khi chuỗi phím cancel hoàn tất;
- buộc GPIO7 về trạng thái an toàn nếu quy trình phần cứng yêu cầu.

### 4.4 MGD không duy nhất toàn hệ thống

`MGD` phải được giữ dạng string để không mất số `0` đầu. Không dùng riêng MGD làm primary key.

Khóa gợi ý:

```text
{gw_pay, node_pay, msg_id}
```

Khóa đối soát hóa đơn:

```text
{gw_pay, node_pay, MGD}
```

### 4.5 Event `input_switch` không có retry/dedupe riêng

Event này chỉ phát tại cạnh GPIO6 `0→1`. Nếu MQTT publish thất bại, firmware không phát lại ở vòng sau. Nếu broker giao lặp do QoS 1, backend phải dedupe.

### 4.6 Phân biệt T7 health-check và T7 phục hồi giao dịch

T7 health-check định kỳ dùng để kiểm tra liên kết RS232 và bị bỏ qua khi có payment active. Riêng T7 phục hồi giao dịch chỉ được gọi sau khi GPIO6 đã về IDLE và cửa sổ chờ hóa đơn đầu tiên hết hạn; kết quả có `Thoi gian` mới được dùng để hoàn tất chính payment đang chờ. `MGD` trong telemetry health-check vẫn không được dùng thay cho MGD của event `completed`.

## 5. Checklist điều tra một giao dịch bị thiếu

1. Có nhận đúng command topic của node không?
2. `msg_id` có đủ 32 ký tự hex không?
3. ACK là `ok` hay `error`?
4. Có log hoàn tất chuỗi phím ảo không?
5. GPIO6 có được chấp nhận ACTIVE không?
6. Có event `input_switch=1` cho cạnh mới không?
7. Có raw ASCII trên RX GPIO11 không?
8. Parser có log đủ MGD, Thoi gian, money, liter, price không?
9. Event publish trả MQTT message ID hay `-1`?
10. Backend có dedupe hoặc gom nhầm topic của nhiều node không?
