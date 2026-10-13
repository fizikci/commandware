do {
    oled init ssd1306
    oled print "Raw A0"
    oled print 0 32 "Percent A0"

    every 200 |
        adc A0 --out raw |
        map 0 1023 0 100 --out percent |
        oled chart $raw --min 0 --max 1023 --y 8 --height 24 |
        oled chart $percent --min 0 --max 100 --y 40 --height 24
}
