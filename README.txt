BulletTimeSA64 v2.2 - bullet time untuk GTA SA Android (AML, arm64)

Build: upload ke GitHub, jalankan workflow "Build BulletTimeSA64", unduh artifact.

Pasang:
  libBulletTimeSA64.so -> /sdcard/Android/data/com.rockstargames.gtasa/mods/
  BulletTime.ini       -> /sdcard/Android/data/com.rockstargames.gtasa/configs/  (hapus .ini lama dulu)

Pakai: tombol bullet time (ikon dua batang) muncul di layar, jalan kaki maupun berkendara.
  Tap sekali = on, tap lagi = off. Tombol hidrolik tidak dipakai/diubah.
  Pindah posisi: TAHAN tombol ~0,6 detik (garis kuning memanjang di atas tombol), bingkai jadi kuning,
  lalu seret ke tempat baru dan lepas. Posisi tersimpan otomatis ke ButtonX / ButtonY di .ini.
  Kunci posisi: ButtonMovable=0. Ukuran: ButtonSize. Lama tahan: ButtonHoldTime.
  Catatan: tombol sekarang aktif saat jari DIANGKAT (tap singkat), bukan saat disentuh.
Log: /sdcard/Android/data/com.rockstargames.gtasa/files/BulletTime_log.txt dan logcat tag "BulletTimeSA64".
Terinspirasi dari "Frnchise Bullet Time" oleh frnchise; kode ditulis ulang dari nol.
