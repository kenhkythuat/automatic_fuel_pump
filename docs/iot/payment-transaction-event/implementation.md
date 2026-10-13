# Nguyên tắc xác định và gửi sự kiện giao dịch

## 1. Mục đích

Tài liệu này mô tả hành vi đang được triển khai trên ESP32 để:

- nhận yêu cầu bơm theo tiền hoặc theo lít từ MQTT;
- nhập giá trị bằng bàn phím ảo;
- xác nhận quá trình bơm đã bắt đầu qua `INPUT_SWITCH` GPIO6;
- nhận và giải mã hóa đơn qua RS232 RX GPIO11;
- gửi sự kiện giao dịch lên topic `event` đúng một lần;
- báo lỗi khi quá trình bơm kết thúc nhưng không nhận được hóa đơn RS232 hợp lệ.

Nguồn hiện thực chính:

- `main/wifi_mqtt_FD.c`: MQTT, payment context, ACK và event.
- `main/virtual_keypad.c`: chuỗi phím ảo.
- `main/rs232.c`: GPIO6/GPIO7, UART RX và parser hóa đơn.
- `main/app_common_interfaces.h`: kiểu dữ liệu và interface dùng chung.

## 2. Thành phần tham gia

| Thành phần | Trách nhiệm |
|---|---|
| Server | Gửi command, nhận ACK và event |
| MQTT broker | Chuyển tiếp message QoS 1 |
| ESP32 | Quản lý trạng thái giao dịch và điều khiển phím ảo |
| GPIO6 `INPUT_SWITCH` | `0 = IDLE`, `1 = ACTIVE` |
| GPIO7 `CONTROL_SWITCH` | Cho phép phần cứng hoạt động theo luồng hiện tại |
| UART2 RX GPIO11 | Nhận chuỗi hóa đơn RS232 |
| Bộ điều khiển trụ bơm | Nhận phím ảo, thực hiện bơm và xuất hóa đơn |

Event `input_switch=1` chỉ được gửi một lần cho mỗi payment sau khi chuỗi phím ảo đã hoàn tất. Nếu GPIO6 đã lên `1` trong lúc nhập phím hoặc đã ở `1` trước đó, firmware lấy lại mức hiện tại ngay sau khi nhập xong, đánh dấu giao dịch bắt đầu và gửi event với `msg_id` của command. Nếu GPIO6 chuyển `0 → 1` sau khi nhập xong, task GPIO gửi cùng event tại cạnh lên.

## 3. MQTT topics

Các ID được đọc từ NVS và đưa vào topic động.

| Topic | Hướng | QoS | Retain | Mục đích |
|---|---|---:|---:|---|
| `tbmq/payment/{gw_pay}/{node_pay}/command` | Server → ESP32 | 1 | Không xác định phía server | Nhận command |
| `tbmq/payment/{gw_pay}/{node_pay}/ack` | ESP32 → Server | 1 | `false` | Xác nhận command được chấp nhận hoặc từ chối |
| `tbmq/payment/{gw_pay}/{node_pay}/event` | ESP32 → Server | 1 | `false` | Báo bắt đầu bơm và kết quả giao dịch |

## 4. Payment context

Tại một thời điểm, firmware chỉ duy trì một payment context đang hoạt động.

| Biến | Ý nghĩa |
|---|---|
| `active_payment_active` | Có command thanh toán đang chờ hoàn tất |
| `active_payment_type` | `MONEY` hoặc `LITTER` |
| `active_payment_msg_id` | Liên kết command, ACK và event |
| `active_payment_money` | Giá trị `qr_money` ban đầu |
| `active_payment_litter` | Giá trị `qr_litter` ban đầu |
| `active_payment_money_keypad_done` | Chuỗi phím ảo đã thực hiện xong |
| `active_payment_input_started` | GPIO6 đã được chấp nhận là ACTIVE ít nhất một lần |

Payment context được bảo vệ bởi mutex `payment_ctx_sem`.

## 5. Nguyên tắc xác định một giao dịch

Một command chỉ có thể trở thành giao dịch hợp lệ khi đi qua các cổng điều kiện sau.

### Bước 1: Command hợp lệ

Command phải là một trong hai loại:

- `set_qr_money`;
- `set_qr_litter`.

`msg_id` bắt buộc là chuỗi 32 ký tự hexadecimal. `param` bắt buộc là object.

Ví dụ theo tiền:

```json
{
  "cmd": "set_qr_money",
  "param": {
    "qr_money": 50000
  },
  "msg_id": "9f1c2ab34d5e4f67a8b9c0d1e2f30014"
}
```

Ví dụ theo lít:

```json
{
  "cmd": "set_qr_litter",
  "param": {
    "qr_litter": 50
  },
  "msg_id": "c9f0f72f96d8405aa2b1f92a69370e4a"
}
```

### Bước 2: Tạo task phím ảo và payment context

Firmware cấp phát tham số, tạo task phím ảo và lưu payment context. Nếu không cấp phát được bộ nhớ hoặc không tạo được task thì ACK trả về `error` và không có giao dịch.

Với `MAIN_RS232 = 1`:

- `set_qr_money`: giữ nguyên `qr_money` khi chuyển thành chuỗi phím;
- `set_qr_litter`: nhập `qr_litter * 10` để bù một chữ số thập phân.

### Bước 3: Gửi ACK

ACK thành công:

```json
{
  "ts": 74696,
  "ack_to": "set_qr_litter",
  "result": "ok",
  "msg_id": "c9f0f72f96d8405aa2b1f92a69370e4a"
}
```

`result = ok` có nghĩa command hợp lệ và task phím ảo đã được tạo. ACK không chứng minh trụ bơm đã nhận phím, chưa chứng minh bơm đã bắt đầu và chưa phải kết quả giao dịch.

### Bước 4: Hoàn tất nhập phím ảo

Sau khi chuỗi phím hoàn tất, firmware đặt:

```text
active_payment_money_keypad_done = true
```

Trong chế độ phím ảo, chỉ từ thời điểm này GPIO7 mới được phép đi theo GPIO6.

### Bước 5: Xác nhận bắt đầu bơm

Giao dịch chỉ được phép hoàn tất nếu:

```text
active_payment_active == true
active_payment_input_started == true
```

`active_payment_input_started` được đặt khi:

1. GPIO6 thực sự chuyển `0 → 1`; hoặc
2. GPIO6 đã ở mức `1` trước command và vẫn là `1` sau khi chuỗi phím ảo hoàn tất.

Khi lần đầu chấp nhận trạng thái ACTIVE, bộ thu hóa đơn RS232 được reset để loại dữ liệu cũ.

Riêng event bắt đầu bơm chỉ được gửi cho cạnh vật lý đã debounce `0 → 1`:

```json
{
  "ts": 74702,
  "event": "input_switch",
  "input_switch": 1,
  "msg_id": "c9f0f72f96d8405aa2b1f92a69370e4a"
}
```

Nếu GPIO6 đã là `1` từ trước command, trạng thái đó vẫn được dùng để xác nhận giao dịch đã bắt đầu nhưng không phát event `input_switch` mới.

### Bước 6: Nhận hóa đơn RS232 hợp lệ

UART2 RX GPIO11 tích lũy ASCII vào buffer 2048 byte. Hóa đơn chỉ được đánh dấu `ready` khi parser lấy đủ cả năm trường:

| Trường RS232 | Kiểu nội bộ | Quy tắc |
|---|---|---|
| `MGD` | Chuỗi tối đa 15 ký tự số | Giữ số `0` ở đầu |
| `Thoi gian` | Chuỗi tối đa 23 ký tự | Dùng nhận diện giao dịch mới, ví dụ `08:34:25 13/07/26` |
| `Thanh tien` | `uint32_t` | Bỏ dấu phân nhóm, lấy các chữ số trước `(` |
| `So lit` | Millilít `uint32_t` | Chấp nhận `,` hoặc `.`; chuẩn hóa ba chữ số thập phân |
| `Don gia` | `uint32_t` | Bỏ dấu phân nhóm, lấy các chữ số trước `(` |

Ví dụ:

```text
MGD: 02687
Thoi gian: 08:34:25 13/07/26
Thanh tien: 30.000(vnd)
So lit: 1,00(lit)
Don gia: 30.000(d/l)
```

Kết quả nội bộ:

```text
MGD = "02687"
transaction_time = "08:34:25 13/07/26"
money = 30000
liter_milliliters = 1000
price = 30000
```

### Bước 7: Claim giao dịch và gửi event kết quả

Khi hóa đơn hợp lệ đến, firmware chỉ claim kết quả nếu payment context còn active và giao dịch đã bắt đầu. Claim và xóa context được thực hiện nguyên tử dưới mutex, vì vậy đường RS232 và đường timeout GPIO6 không thể gửi hai event `completed` cho cùng một context.

Event thành công:

```json
{
  "ts": 74715,
  "event": "completed",
  "msg_id": "c9f0f72f96d8405aa2b1f92a69370e4a",
  "MGD": "02687",
  "money": 30000,
  "liter": 1.000,
  "price": 30000
}
```

Hóa đơn RS232 hợp lệ được phép hoàn tất giao dịch ngay, không cần chờ GPIO6 trở về `0`.

Sau khi MQTT publish trả về thành công, firmware lưu giao dịch cuối vào namespace NVS `nodeconfig`:

```text
lastTxnTime  = Thoi gian
lastTxnMoney = Thanh tien
lastTxnLiter = So lit tính theo millilít
```

`Thoi gian` là trường quyết định hóa đơn mới hay trùng. Tiền và lít được lưu để debug/đối soát nhưng được phép giống giao dịch trước.

### Bước 8: Kết thúc qua GPIO6 nhưng chưa có RS232

Nếu giao dịch đã bắt đầu và GPIO6 chuyển `1 → 0`, firmware chờ hóa đơn tối đa:

```text
RS232_RECEIPT_WAIT_MS = 7000 ms
RS232_RECEIPT_POLL_MS = 50 ms
```

Nếu hóa đơn mới hợp lệ xuất hiện trong thời gian này, firmware gửi event thành công. Nếu hết 7 giây vẫn chưa có hóa đơn mới hợp lệ, firmware chưa báo lỗi mà chuyển sang bước phục hồi T7.

### Bước 9: Phục hồi giao dịch bằng T7

Firmware giữ nguyên payment context đang chờ và thực hiện:

1. Reset RS232 receipt collector.
2. Nhấn chuỗi phím ảo `T7` để yêu cầu trụ xuất giao dịch gần nhất.
3. Chờ thêm tối đa 7000 ms, poll mỗi 50 ms.
4. Parse đủ năm trường, bao gồm `Thoi gian`.
5. Đọc tuple giao dịch cuối đã publish trong NVS.
6. Chỉ chấp nhận hóa đơn khi `Thoi gian` khác `lastTxnTime`. `money` và `liter` được phép trùng.
7. Nếu publish thành công, cập nhật tuple mới vào NVS.

T7 ở bước này là luồng phục hồi payment, không phải luồng health-check RS232 định kỳ.

### Bước 10: Chỉ báo lỗi sau khi T7 thất bại

Nếu không phát được T7, T7 không trả dữ liệu, dữ liệu không đủ năm trường, hoặc T7 chỉ trả lại đúng `lastTxnTime`, firmware chờ hết cửa sổ 7 giây thứ hai rồi mới claim context và gửi:

```json
{
  "ts": 74722,
  "event": "completed",
  "msg_id": "c9f0f72f96d8405aa2b1f92a69370e4a",
  "result": "error",
  "description": "missing_rs232_receipt",
  "money": 0,
  "liter": 0,
  "price": 30490
}
```

## 6. Điều kiện tối thiểu của event `completed`

Event kết quả chỉ được phát khi đồng thời có:

```text
1. Command set_qr_money hoặc set_qr_litter đã tạo payment context thành công.
2. Payment context chưa bị cancel hoặc claim bởi luồng khác.
3. GPIO6 đã được chấp nhận ACTIVE ít nhất một lần.
4a. Có hóa đơn RS232 hợp lệ; hoặc
4b. GPIO6 đã về IDLE, hết 7 giây chờ ban đầu, đã thử T7 và hết thêm 7 giây mà vẫn không có hóa đơn mới để phát kết quả lỗi.
```

Dữ liệu RS232 tự do khi không có giao dịch active hoặc trước khi giao dịch được đánh dấu started không được phép tạo event giao dịch.

## 7. Hủy giao dịch

Command hiện tại dùng tên `cancel_qr_money` cho cả payment context tiền và lít.

Khi nhận command cancel hợp lệ, `msg_id` được dùng để liên kết ACK; payment context đang active được hủy không phụ thuộc mức GPIO6:

- đánh dấu dừng chuỗi nhập tiền/lít đang chạy ở ranh giới giữa hai lần nhấn phím;
- xóa payment context;
- reset collector RS232;
- dừng timer điều khiển và ép GPIO7 về `0` ngay lập tức, không phụ thuộc GPIO6;
- tạo task nhấn phím ảo `CC`;
- gửi ACK cancel `ok`;
- GPIO6 hoặc hóa đơn RS232 đến sau đó không tạo event `completed` cho context đã xóa.

ACK cancel hiện xác nhận task cancel `CC` được tạo; GPIO7 đã được ép xuống `0` trước ACK nhưng ACK không xác nhận phần cứng đã thực sự nhận đủ hai phím `C`.

## 8. Idempotency và tính duy nhất

- `msg_id` là khóa tương quan chính giữa command, ACK và event.
- `MGD` là số giao dịch cục bộ do trụ bơm trả về; không được giả định duy nhất toàn hệ thống.
- Backend nên dùng ít nhất `{node_pay, msg_id}` để định danh command.
- Khi cần đối soát hóa đơn, dùng `{node_pay, MGD}` hoặc `{node_pay, msg_id, MGD}`.
- Topic chứa `gw_pay` và `node_pay`; payload event hiện không chứa `DevID`, vì vậy backend phải giữ lại danh tính từ topic.
- MQTT QoS 1 có thể giao message lặp. Backend phải xử lý event theo hướng idempotent.

## 9. Ghi chú GPIO7

- Chế độ bàn phím thật: GPIO7 đi theo GPIO6 theo logic hiện tại.
- Chế độ bàn phím ảo: GPIO7 chỉ được phép lên `1` sau khi chuỗi phím thanh toán hoàn tất.
- Sau event thành công, nếu GPIO6 vẫn là `1`, timer sẽ buộc GPIO7 về `0` sau 5 giây.
- Event lỗi `missing_rs232_receipt` hiện không lập lịch delayed-off theo nhánh thành công.

## 10. Quan sát và log chuẩn

Một giao dịch thành công thường có chuỗi log:

```text
Payment command accepted: cmd=set_qr_litter ... msg_id=...
set_qr_litter virtual keypad done ...
INPUT_SWITCH GPIO6: 0 -> 1 (ACTIVE)
Payment input switch ACTIVE: dispense started ...
Payment INPUT_SWITCH ACTIVE event published ...
[RS232 RX ASCII] ...
RS232 receipt parsed: MGD=... money=... liter=... price=...
Use RS232 receipt immediately ...
Payment completion triggered by RS232 receipt ...
completion event publish stat=...
```

## 11. Test cases tối thiểu

| ID | Kịch bản | Kết quả mong đợi |
|---|---|---|
| T01 | Command hợp lệ, GPIO6 `0→1`, RS232 hợp lệ | ACK `ok`, event `input_switch=1`, một event `completed` thành công |
| T02 | GPIO6 đã là `1` trước command, phím ảo xong, RS232 hợp lệ | Không có event cạnh mới; vẫn có event `completed` thành công |
| T03 | RS232 hợp lệ đến trước GPIO6 về `0` | Gửi `completed` ngay |
| T04 | GPIO6 `1→0`, RS232 đến trong 7 giây | Gửi `completed` thành công |
| T05 | GPIO6 `1→0`, không có RS232 trong 7 giây đầu, T7 trả hóa đơn mới | Gửi `completed` thành công và lưu tuple mới vào NVS |
| T06 | GPIO6 `1→0`, không có RS232; T7 cũng timeout thêm 7 giây | Gửi `missing_rs232_receipt`, money/liter bằng 0 |
| T07 | T7 trả cùng `Thoi gian` đã lưu nhưng money/liter giống hoặc khác | Xem là hóa đơn cũ; không hoàn tất, chờ hết timeout rồi báo missing |
| T08 | RS232 đến khi chưa có payment active | Không gửi event giao dịch |
| T09 | RS232 đến trước khi GPIO6 từng ACTIVE | Không claim giao dịch |
| T10 | Cancel đúng `msg_id` trước khi hoàn tất | Xóa context; không gửi `completed` cho giao dịch đó |
| T11 | MQTT QoS 1 giao lặp event | Backend không tạo hai bản ghi cho cùng `{node_pay,msg_id,event}` |
| T12 | Hai thiết bị có cùng MGD | Backend vẫn phân biệt bằng `node_pay` và `msg_id` |

## 12. Giả định

- `ts` lấy từ `time(NULL)`. Backend không nên coi đây là UTC chính xác nếu thiết bị chưa đồng bộ thời gian.
- Server giữ lại `gw_pay` và `node_pay` từ topic khi lưu event.
- Mỗi ESP32 chỉ xử lý một payment context tại một thời điểm.
- Tài liệu mô tả code hiện tại, không phải cam kết rằng trụ bơm vật lý chắc chắn đã thực thi chỉ vì ACK trả `ok`.
