# FriKey — Vietnamese Input by Friday

Bộ gõ tiếng Việt cho Windows 11, kèm chế độ dịch tin nhắn khi chat.

- Gõ Telex, tự chuyển Anh/Việt, nhớ ngôn ngữ theo ứng dụng và trang web, bảng emoji (từ Vikey).
- Chế độ dịch: gõ tiếng Việt, bấm **Alt+T**, đoạn vừa gõ được thay bằng bản dịch ngay tại chỗ; đọc lại rồi Enter gửi.
  **Backspace** ngay sau đó lấy lại câu gốc. **Alt+Shift+T** bật/tắt. Dịch qua một API kiểu OpenAI (Gemini, OpenAI…).
- Icon ở khay: bấm 1 lần chuyển Tiếng Việt/English, bấm đúp mở Cài đặt, bấm phải mở menu.

## Bảo mật

Chỉ đoạn cần dịch được gửi đi, khi bạn bấm phím dịch, qua HTTPS tới đúng máy chủ đã cài. Không gửi gì khi chế độ dịch tắt,
không dịch trong ô mật khẩu, trình quản lý mật khẩu hay terminal, không ghi nội dung gõ ra đĩa. API key nằm trong Windows
Credential Manager. Không có tự cập nhật, không gửi báo lỗi.

## Build

Mỗi lần đẩy lên `main`, GitHub Actions build `FriKey.exe` (x64) bằng MSVC, chạy bộ kiểm tra engine gõ, và đưa
file vào mục Artifacts của lần chạy. Đẩy tag `v*` thì tạo bản phát hành. Build tại máy:

```
cmake -S Windows -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target vikey
```

## Nguồn gốc và giấy phép

FriKey phát triển từ [Vikey](https://github.com/tonyminibot/Vikey) của Tony Dinh (bản 1.0.33, commit `985bbff`),
dựa trên [OpenKey](https://github.com/tuyenvm/OpenKey) của Tuyen Mai. Giấy phép GNU GPL v3.0 (xem `LICENSE`).
Repo này là bản fork của Vikey: các thư mục iOS, Android, Linux, macOS vẫn là của Vikey, FriKey chỉ build bản Windows
(`Windows/`, dùng chung `Engine/`). Các workflow của Vikey đã được gỡ; chỉ còn `.github/workflows/build.yml`.
Font [Plus Jakarta Sans](https://github.com/tokotype/PlusJakartaSans): SIL Open Font License 1.1 (`Windows/App/Fonts/OFL.txt`).
