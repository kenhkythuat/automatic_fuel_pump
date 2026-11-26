# Luồng runtime

```mermaid
flowchart TD
    A[Nhận cancel hợp lệ] --> B[Khóa GPIO7 thấp và dừng nhập]
    B --> C{Payment đã started?}
    C -->|Chưa| D[Đóng payment và nhấn CC]
    C -->|Đã| E[Giữ msg_id gốc và collector, nhấn CC]
    E --> F[Chờ hóa đơn 7 giây]
    F -->|Thiếu| G[T7 và chờ thêm 7 giây]
    F -->|Hóa đơn mới| H[Commit NVS: active thành event]
    G -->|Hóa đơn mới| H
    G -->|Timeout| I[Tạo completed lỗi với tiền và lít 0]
    I --> H
    H --> J[Worker enqueue QoS1]
    J --> K{PUBACK đúng packet và thành công?}
    K -->|Có| L[Commit xóa event khỏi NVS]
    K -->|Chưa| J
```

ACK cancel được gửi sau khi tạo task CC, không đợi hóa đơn. Nếu hóa đơn đến trong khi CC đang nhấn, kết quả vẫn được lưu. Reboot nạp lại journal: gửi các event tồn đọng và phục hồi payment started với GPIO7 thấp. Command mới bị chặn khi đang phục hồi hoặc queue đầy.
