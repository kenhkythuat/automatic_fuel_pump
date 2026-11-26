# Cancel và bảo toàn kết quả payment

Tài liệu này thay thế các mô tả cũ về xóa payment ngay khi cancel và xóa event ngay khi publish trả về thành công.

## MQTT contract

Các topic giữ nguyên: `tbmq/payment/{gw_pay}/{node_pay}/command`, `/ack`, `/event`.
Command tiền/lít và cancel giữ nguyên JSON hiện tại. ACK cancel liên kết `msg_id` của cancel; event `completed` dùng `msg_id` payment gốc. Không thêm trường JSON mới.

## Cancel

- GPIO7 được khóa thấp ngay khi nhận cancel hợp lệ, kể cả GPIO6 vẫn cao.
- Hủy nhập tiền/lít tại ranh giới phím, thực hiện `CC`. Nếu đang dùng route phím thật, chuyển route tạm thời cho CC rồi trả lại.
- Chưa xác nhận bắt đầu bơm: đóng payment, không phát completed.
- Đã xác nhận bắt đầu bơm: giữ payment và collector RS232. Hóa đơn có đủ MGD, Thoi gian, tiền, lít, giá được lưu thành event ngay, không chờ GPIO6 xuống thấp.
- Sau CC, nếu còn payment: chờ tối đa 7 giây; thiếu hóa đơn mới thì gọi T7 phục hồi, chờ thêm 7 giây. Chỉ sau đó tạo lỗi missing_rs232_receipt với money/liter bằng 0.
- Cancel lặp lại ACK nhưng không reset collector hay thời hạn chờ. Nếu vòng chờ GPIO6 đã chạy trước cancel, giữ vòng chờ đó.
- Khi active/cancel task/receipt wait còn chạy, command payment mới trả error busy. Đổi giá/route cũng bị chặn trong thời gian này để tránh chen chuỗi phím.

## NVS journal

Module `main/payment_journal.c` lưu blob `pay_journal/state` trong phân vùng NVS hiện tại, không đổi partition table. Blob có version, payment active, 4 slot event, thông tin hóa đơn gần nhất đã PUBACK.

1. Lưu payment trước khi tạo task phím ảo. Lưu started trước khi cho GPIO7 bật.
2. Khi có kết quả, lưu event và xóa active trong cùng một commit NVS. Chỉ sau commit thành công mới bỏ active trong RAM.
3. Worker gửi lần lượt FIFO bằng MQTT QoS1. Mỗi event lưu nguyên topic và payload.
4. Chỉ PUBACK đúng packet ID và không có reason lỗi mới cho worker xóa event bằng commit NVS.
5. Mỗi 30 giây chưa có PUBACK, worker thử enqueue lại. Mất mạng không xóa event.
6. Sau reboot, khôi phục event và gửi lại khi MQTT kết nối. Payment started được khóa GPIO7 thấp và phục hồi hóa đơn; không replay nhập tiền/lít. Payment chưa started được đóng, không tự khởi động bơm.
7. So sánh Thoi gian với cả hóa đơn đang chờ gửi và hóa đơn đã PUBACK để loại hóa đơn T7 cũ. Tiền/lít được phép trùng.

NVS đầy/lỗi: chặn payment mới, giữ trạng thái chưa hoàn tất; nếu hóa đơn thật đã nhận nhưng commit thất bại, giữ bản sao RAM và thử lưu lại. Khởi động không tự erase NVS khi init lỗi. Không thể bảo đảm lưu mới khi flash hỏng vật lý; lỗi luôn được ghi log.

## Backend và giới hạn

PUBACK chỉ xác nhận broker, chưa xác nhận lưu database. Backend phải dedupe theo device + payment msg_id + event. Mất điện sau broker nhận nhưng trước commit xóa có thể khiến gửi lại. Không xóa flash/NVS và không nạp đè nvs.bin nếu muốn giữ journal; lệnh flash đầy đủ hiện tại của project có thể nạp nvs.bin. OTA app không ghi đè NVS.

Queue bền vững áp dụng cho kết quả `completed`; ACK command, telemetry và event cạnh input_switch vẫn dùng đường gửi hiện tại. Journal giữ 4 kết quả chưa ACK, không phải lịch sử giao dịch vô hạn.

## Kiểm thử

Build ESP-IDF và kiểm tra kích thước app. Các thử nghiệm tích hợp trong exceptions.md cần thực hiện trên thiết bị/broker; build không xác nhận được mất điện thực tế hay thời điểm in hóa đơn.
