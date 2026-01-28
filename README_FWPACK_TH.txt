วิธีทำ “อัปเดตไฟล์เดียว” (FWPACK) สำหรับโปรเจกต์คุณ ✅

ไฟล์ใน build/ ที่ใช้:
  1) build\my_app.bin      = แอปเฟิร์มแวร์ (OTA app)
  2) build\storage.bin     = SPIFFS image (หน้าเว็บ index/app/style)

1) วางทับโค้ดให้รองรับ FWPACK
  - main\portal_wifi.c  (ใน zip นี้)
  - spiffs\app.js       (ใน zip นี้ — ให้เลือก .fwpack ได้)
  แล้ว build ใหม่: idf.py build

2) อัปเดตครั้งแรก (เพื่อให้เครื่อง “รู้จัก fwpack”)
  - เปิดหน้าเว็บ 192.168.4.1 -> firmware
  - เลือก build\my_app.bin แล้ว Upload (แบบเดิม)

3) สร้างไฟล์เดียวสำหรับลูกค้า (fwpack)
  - รันจากโฟลเดอร์โปรเจกต์:
      python tools\make_fwpack.py --app build\my_app.bin --spiffs build\storage.bin --out build\footswitch.fwpack

4) จากนี้ไป ลูกค้าอัป “ไฟล์เดียว” ได้เลย
  - เลือก build\footswitch.fwpack แล้ว Upload
  - จะอัปทั้ง APP + หน้าเว็บ (SPIFFS) ในครั้งเดียว แล้วรีบูตเอง

หมายเหตุ
  - ถ้าอยากให้ลูกค้า “เห็นแค่ไฟล์เดียว” จริงๆ คุณสามารถตั้งชื่อ fwpack เป็น .bin ได้
    เช่น build\update.bin (ตัวเครื่องจะดู magic "FWPK" ไม่สนสกุลไฟล์)
