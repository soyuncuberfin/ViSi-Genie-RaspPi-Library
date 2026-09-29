import serial
import time
import sys

# --- AYARLAR ---
PORT = "/dev/ttyUSB0"
BAUD = 9600

# ViSi-Genie Standart Komut ve Nesne Sabitleri
GENIE_WRITE_OBJ    = 0x01
GENIE_REPORT_EVENT = 0x07
GENIE_OBJ_FORM     = 0x0A
GENIE_OBJ_USERBTN  = 0x21  # Ekranınızdan gelen UserButton nesnesi


def calculate_checksum(cmd, obj_type, obj_idx, val_msb, val_lsb):
    """ViSi-Genie XOR Checksum hesaplama"""
    return cmd ^ obj_type ^ obj_idx ^ val_msb ^ val_lsb


def change_form(ser, target_form_index):
    """
    ViSi-Genie standardında form değiştirme:
    Paket Yapısı: [CMD, OBJECT_TYPE, OBJECT_INDEX, VALUE_MSB, VALUE_LSB, CHECKSUM]
    Object: GENIE_OBJ_FORM (0x0A)
    Index:  target_form_index (0: Form0, 1: Form1)
    Value:  0x0000
    """
    val_msb = 0x00
    val_lsb = 0x00
    chk = calculate_checksum(GENIE_WRITE_OBJ, GENIE_OBJ_FORM, target_form_index, val_msb, val_lsb)
    
    packet = bytearray([GENIE_WRITE_OBJ, GENIE_OBJ_FORM, target_form_index, val_msb, val_lsb, chk])
    ser.write(packet)
    ser.flush()
    print(f"\n-> [KOMUT GÖNDERİLDİ] Form {target_form_index} istendi (Paket: {[hex(b) for b in packet]})")


def main():
    print(f"Porta bağlanılıyor: {PORT} ({BAUD} baud)...")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.1, dsrdtr=False, rtscts=False)
        ser.dtr = False
        ser.rts = False
        ser.reset_input_buffer()
        ser.reset_output_buffer()
    except Exception as e:
        print(f"\nHATA: Porta bağlanılamadı -> {e}")
        sys.exit(1)

    current_form = 0
    last_trigger_time = 0
    COOLDOWN_SECONDS = 0.4  # Tek dokunuşta peş peşe tetiklenmeyi (bounce) engeller

    print("\n--- Test Başlatıldı ---")
    print("Ekrandaki butona dokunun... (Çıkmak için: Ctrl + C)\n")

    try:
        while True:
            if ser.in_waiting > 0:
                header = ser.read(1)
                if len(header) > 0 and header[0] == GENIE_REPORT_EVENT:
                    data = ser.read(5)
                    if len(data) == 5:
                        obj_type = data[0]
                        obj_idx  = data[1]
                        val      = (data[2] << 8) | data[3]
                        chk      = data[4]
                        
                        expected_chk = GENIE_REPORT_EVENT ^ obj_type ^ obj_idx ^ data[2] ^ data[3]
                        if chk == expected_chk:
                            print(f"<- [EVENT ALINDI] Nesne: {obj_type:#04x}, İndeks: {obj_idx}, Değer: {val}")
                            
                            # 0x21 (UserButton) basış algılandığında (Değer 0 veya 1 fark etmeksizin)
                            now = time.time()
                            if obj_type == GENIE_OBJ_USERBTN and (now - last_trigger_time) > COOLDOWN_SECONDS:
                                last_trigger_time = now
                                current_form = 1 if current_form == 0 else 0
                                change_form(ser, current_form)
                        else:
                            print("<- [UYARI] Bozuk veri / Checksum hatası.")
            time.sleep(0.01)

    except KeyboardInterrupt:
        print("\nTest durduruldu.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()