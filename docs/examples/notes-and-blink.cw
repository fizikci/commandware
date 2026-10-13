func notes str {
    split " " $str |
        each --out note {
            split ":" $note --out pitch,dur |
                tone 15 --hz notehz($pitch) --dur int($dur)
        }
}

do {
    every 500 | toggle | write 12
    call notes "C4:200 E4:300"
    print "Melody finished; LED continues blinking"
}
