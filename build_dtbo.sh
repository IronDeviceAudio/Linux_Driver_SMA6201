dtc -W no-unit_address_vs_reg -@ -I dts -O dtb -o sma6201-i2s-soundcard.dtbo sma6201-i2s-soundcard-overlay.dts
sudo cp sma6201-i2s-soundcard.dtbo /boot/overlays