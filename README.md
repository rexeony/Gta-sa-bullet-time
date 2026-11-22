# BulletTimeSA64 v2.0

Mod AML (arm64) untuk GTA SA Android: bullet time + dodge ala Max Payne, dipicu tombol hidrolik.

## Cara pakai di game
- Jalan kaki: tombol hidrolik muncul. **Tap 1x** = bullet time on/off. **Tap 2x cepat** = dodge.
- Arah dodge mengikuti stik kiri (maju / mundur / kiri / kanan). Animasi `sd_dive_*` dari MP3SD.ifp,
  varian 1 tangan / 2 tangan / dual dipilih otomatis dari senjata yang dipegang.
- Dodge bisa dilakukan saat sedang bidik maupun tidak. Kalau belum bidik, otomatis masuk mode bidik selama melayang; menembak tetap dari tombol tembak (atau `AutoFireDuringDive=1`).
- Bar adrenalin: terkuras saat bullet time dan terisi dengan menunggu (kill tidak mengisi, bisa diaktifkan lewat KillRefill).

## Build (tanpa PC)
1. Upload folder ini ke repo GitHub, tab Actions -> "Build BulletTimeSA64" -> Run workflow.
2. Download artifact `BulletTimeSA64`, salin isinya ke folder game:
   - `mods/libBulletTimeSA64.so` -> `/sdcard/Android/data/com.rockstargames.gtasa/mods/`
   - `configs/BulletTime.ini`   -> `/sdcard/Android/data/com.rockstargames.gtasa/configs/`
3. Salin `MP3SD.ifp` (dari mod Frnchise Bullet Time) ke `/sdcard/Android/data/com.rockstargames.gtasa/files/BulletTime/MP3SD.ifp`.

Log ada di `BulletTime_log.txt` (folder AMLModLogs). Baris "Cek versi offset: COCOK" harus muncul.

## Catatan teknis (hasil baca libGTASA.so)
- Penyebab tombol hidrolik mati saat jalan kaki: `CWidgetButtonHydraulics::Update()` bawaan keluar lebih awal
  kalau tidak ada kendaraan dan menimpa flag tampil (`+0xC9`) menjadi 0; widget juga bisa nonaktif (`+0x59`).
  Mod ini mengisi keduanya kembali setiap frame.
- Offset yang dipakai (semua dicek saat load lewat prolog fungsi; kalau versi beda, hook tidak dipasang):
  CEntity matrix `+0x18`, clump `+0x20`; CPhysical move speed `+0x68`; CPed health `+0x6AC`,
  senjata `+0x730` (stride 0x20), slot aktif `+0x8DC`; CPad disabled `+0x110`; widget enabled `+0x59`.
- Kill terdeteksi lewat `CPedDamageResponseCalculator::ComputeWillKillPed`.
