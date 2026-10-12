# Luồng giao dịch và MQTT event

## 1. Luồng thành công chuẩn

```mermaid
sequenceDiagram
    participant S as Server
    participant M as MQTT Broker
    participant E as ESP32
    participant K as Virtual Keypad
    participant P as Pump Controller
    participant U as RS232 UART

    S->>M: set_qr_money hoặc set_qr_litter + msg_id
    M->>E: command (QoS 1)
    E->>E: Validate cmd, param, msg_id
    E->>E: Create keypad task và active payment context
    E->>M: ACK result=ok + msg_id
    M->>S: ACK
    E->>K: Thực hiện chuỗi phím ảo
    K->>P: Giá trị tiền hoặc lít
    K-->>E: Keypad sequence done
    E->>E: Cho phép GPIO7 đi theo GPIO6
    P-->>E: GPIO6 0 → 1
    E->>E: active_payment_input_started=true
    E->>E: Reset RS232 receipt collector
    E->>M: event=input_switch, input_switch=1, msg_id
    M->>S: Event bắt đầu bơm
    P->>U: Hóa đơn ASCII
    U->>E: MGD, Thanh tien, So lit, Don gia
    E->>E: Parse đủ 4 trường
    E->>E: Claim và xóa payment context nguyên tử
    E->>M: event=completed + msg_id + MGD + kết quả thực tế
    M->>S: Event hoàn tất
```

Ghi chú:

1. ACK `ok` chỉ xác nhận task đã được tạo.
2. GPIO6 ACTIVE là bằng chứng giao dịch đã bắt đầu.
3. Hóa đơn hợp lệ có thể hoàn tất giao dịch ngay, không cần chờ GPIO6 xuống `0`.
4. Claim context bảo đảm chỉ một đường được quyền gửi kết quả.

## 2. State machine

```mermaid
flowchart TD
    A[Không có giao dịch] -->|Command hợp lệ và tạo task thành công| B[PAYMENT_ACTIVE]
    A -->|Command lỗi| AE[ACK error]
    B -->|ACK ok| C[Chờ phím ảo hoàn tất]
    C -->|Keypad done| D[Chờ hoặc kiểm tra GPIO6 ACTIVE]
    D -->|GPIO6 0→1| E[PAYMENT_STARTED]
    D -->|GPIO6 đã là 1 từ trước| E
    E -->|Cạnh vật lý 0→1| ES[Gửi event input_switch=1]
    ES --> F[Chờ hóa đơn hoặc GPIO6 IDLE]
    E --> F
    F -->|RS232 đủ 4 trường| G[Claim context]
    G --> H[Gửi completed thành công]
    F -->|GPIO6 1→0| I[Chờ RS232 tối đa 7 giây]
    I -->|Có hóa đơn hợp lệ| G
    I -->|Hết 7 giây| J[Claim context]
    J --> K[Gửi completed lỗi missing_rs232_receipt]
    B -->|Cancel đúng msg_id| L[Xóa context và nhấn C]
    C -->|Cancel đúng msg_id| L
    D -->|Cancel đúng msg_id| L
    E -->|Cancel đúng msg_id| L
    H --> A
    K --> A
    L --> A
```

## 3. GPIO6 đã ACTIVE trước command

```mermaid
sequenceDiagram
    participant P as Pump Controller
    participant E as ESP32
    participant S as Server

    P-->>E: GPIO6 đã ở mức 1
    S->>E: Command + msg_id
    E-->>S: ACK ok
    E->>E: Thực hiện phím ảo
    E->>E: Đọc lại GPIO6 = 1
    E->>E: Đánh dấu payment started và reset RS232 collector
    Note over E,S: Không gửi event input_switch vì không có cạnh mới 0→1
    P-->>E: Hóa đơn RS232
    E-->>S: event completed
```

Điểm quan trọng: trạng thái `1` có sẵn vẫn thỏa điều kiện bắt đầu giao dịch, nhưng không được giả thành một cạnh mới để gửi event `input_switch`.

## 4. GPIO6 về IDLE trước khi hóa đơn tới

```mermaid
sequenceDiagram
    participant P as Pump Controller
    participant E as ESP32
    participant S as Server

    P-->>E: GPIO6 0→1
    E-->>S: event input_switch=1
    P-->>E: GPIO6 1→0
    E->>E: Poll receipt mỗi 50 ms, tối đa 7000 ms
    alt RS232 hợp lệ trong thời gian chờ
        P-->>E: Hóa đơn RS232
        E-->>S: completed thành công
    else Không có hóa đơn hợp lệ
        E-->>S: completed + missing_rs232_receipt
    end
```

## 5. Hóa đơn tới trước GPIO6 IDLE

```mermaid
sequenceDiagram
    participant P as Pump Controller
    participant E as ESP32
    participant S as Server

    P-->>E: GPIO6 0→1
    E-->>S: event input_switch=1
    P-->>E: Hóa đơn RS232 hoàn chỉnh
    E->>E: Parse và claim active context
    E-->>S: completed thành công ngay
    P-->>E: GPIO6 1→0 sau đó
    E->>E: Bỏ qua vì context đã hoàn tất
```

## 6. Luồng cancel

```mermaid
sequenceDiagram
    participant S as Server
    participant E as ESP32
    participant K as Virtual Keypad

    S->>E: cancel_qr_money + cùng msg_id
    E->>E: Tạo cancel task
    E->>E: Xóa active payment context nếu msg_id khớp
    E-->>S: ACK cancel result=ok
    E->>K: Nhấn C
    Note over E: RS232 hoặc GPIO6 đến sau không còn context để tạo completed
```

## 7. Quy tắc phía server

1. Đọc danh tính thiết bị từ topic `{gw_pay}/{node_pay}`.
2. Dùng `msg_id` để nối command → ACK → input-switch event → completed event.
3. Không đánh dấu giao dịch hoàn tất chỉ dựa trên ACK.
4. Chỉ event `completed` mới là kết quả cuối.
5. Xử lý QoS 1 theo hướng idempotent vì message có thể lặp.
6. Không dùng riêng `MGD` làm khóa toàn hệ thống.
