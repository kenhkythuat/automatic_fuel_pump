# Ca kiểm tra tích hợp và lỗi

| Ca | Kết quả phải thấy |
|---|---|
| Cancel khi đang nhập, chưa started | GPIO7 thấp, dừng digits/E, CC, ACK; không completed |
| Đang bơm, cancel và in MGD 02812 tiền 3700 lít 0.37 | Completed đúng giá trị thực và msg_id payment gốc |
| Hóa đơn đến lúc đang nhấn CC | Không reset collector; kết quả lưu NVS |
| Cancel gửi lặp trong lúc chờ | Không tạo CC mới, không kéo dài thời gian chờ |
| Payment mới chen vào lúc phục hồi | ACK error busy, không nhấn thêm phím |
| Thiếu RS232 sau CC | 7 giây, T7, thêm 7 giây rồi mới missing_rs232_receipt |
| T7 trả cùng Thoi gian pending/đã ACK | Không gán hóa đơn cũ cho payment |
| Wi-Fi tắt trước khi completed | Log Completion saved before MQTT; gửi khi kết nối lại |
| Reboot trước enqueue hoặc trước PUBACK | Khôi phục pending, gửi lại nguyên topic/payload |
| PUBACK packet ID không thuộc event | Không xóa NVS event |
| PUBACK MQTT5 reason >= 0x80 | Log Broker rejected; giữ event |
| Reboot sau PUBACK nhưng trước xóa NVS | Có thể gửi lặp; backend chỉ ghi một giao dịch |
| Reboot khi payment started chưa có hóa đơn | GPIO7 thấp, phục hồi hóa đơn/T7; không replay set tiền/lít |
| NVS commit event thất bại | Giữ context, khóa GPIO7 thấp, giữ hóa đơn RAM để thử lại |
| Queue đủ 4 kết quả chưa ACK | Từ chối payment mới; gửi dần FIFO sau reconnect |
| NVS init lỗi | Không tự erase flash; dừng startup |

Log chính: `CANCEL_WAIT_RECEIPT`, `Completion saved before MQTT`, `Enqueued durable event packet=`, `Broker PUBACK packet=`, `Restored pending=`, `NVS commit failed`.

Nếu mất điện trước khi bất kỳ dữ liệu hóa đơn nào được lưu thành công và trụ không còn trả được giao dịch qua T7, firmware không thể tái tạo số tiền/lít thực tế; kết quả phải là lỗi thiếu hóa đơn.
