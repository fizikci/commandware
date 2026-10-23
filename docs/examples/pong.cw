# Two players, SSD1306 128x64 at 0x3c (change init for SH1106).
# OLED: SDA GPIO4, SCL GPIO5; 3.3V bus pull-ups and common GND.
# Left button: GPIO12 to GND; right: GPIO14 to GND.
# Each button needs an external 10k pull-up to 3.3V.
# Hold to move up, release to move down. Stop old jobs before running.
# 20 frames/s; best-effort timing. Scores wrap after 99.

func serve direction {
    set ballX 63
    set ballY 35
    set velocityX $direction
    set velocityY 1
    set waiting 16
    oled rect 40 0 48 8 --value 0
    oled print 40 0 "$leftScore:$rightScore"
}

func draw {
    oled rect 1 $leftY 3 14
    oled rect 124 $rightY 3 14
    oled rect $ballX $ballY 2 2
}

func bounce paddleTop {
    # Keep expressions shallow for the ESP8266's small interpreter stack.
    set velocityY $ballY - $paddleTop - 6
    set velocityY $velocityY / 3
    set velocityY int $velocityY
    set velocityY clamp $velocityY (-2) 2
    if $velocityY == 0 { set velocityY 1 }
}

oled init ssd1306 --height 64
oled rect 0 8 128 1
set leftY 28
set rightY 28
set leftScore 0
set rightScore 0
serve 2
draw

every 50 | do {
    # Erase the previous sprites, then update both players in one job.
    oled rect 1 $leftY 3 14 --value 0
    oled rect 124 $rightY 3 14 --value 0
    oled rect $ballX $ballY 2 2 --value 0
    read 12 | if $value == 0 {
        set leftY max 10 ($leftY - 2)
    } else {
        set leftY min 50 ($leftY + 2)
    }
    read 14 | if $value == 0 {
        set rightY max 10 ($rightY - 2)
    } else {
        set rightY min 50 ($rightY + 2)
    }
    if $waiting > 0 {
        set waiting $waiting - 1
    } else {
        set ballX $ballX + $velocityX
        set ballY $ballY + $velocityY
        if $ballY < 10 {
            set ballY 10
            set velocityY abs $velocityY
        }
        if $ballY > 62 {
            set ballY 62
            set velocityY -(abs $velocityY)
        }
        if $velocityX < 0 && $ballX <= 4 && $ballX >= 2 && $ballY + 2 > $leftY && $ballY < $leftY + 14 {
            set ballX 4
            set velocityX 2
            bounce $leftY
        }
        if $velocityX > 0 && $ballX >= 122 && $ballX <= 124 && $ballY + 2 > $rightY && $ballY < $rightY + 14 {
            set ballX 122
            set velocityX (-2)
            bounce $rightY
        }
        if $ballX < 0 {
            set rightScore ($rightScore + 1) % 100
            serve (-2)
        }
        if $ballX > 126 {
            set leftScore ($leftScore + 1) % 100
            serve 2
        }
    }
    draw
}
