#!/bin/sh
# Test double for Apple's /System/Library/Printers/Libraries/snmpInk: prints a
# Printer MIB supply table plist chosen by the queue name (the only argument).
# Sahte snmpInk: kuyruk adına göre Printer MIB sarf malzemesi plist'i yazar.
[ "$#" -eq 1 ] || { echo "snmpInk: A printer id must be supplied." >&2; exit 0; }

supply() {
    cat <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>prtMarkerSuppliesTable</key>
	<array>
		<dict>
			<key>prtMarkerSuppliesClass</key>
			<integer>3</integer>
			<key>prtMarkerSuppliesDescription</key>
			<string>Black Cartridge HP CE278A</string>
			<key>prtMarkerSuppliesIndex</key>
			<integer>1</integer>
			<key>prtMarkerSuppliesLevel</key>
			<integer>$1</integer>
			<key>prtMarkerSuppliesMaxCapacity</key>
			<integer>$2</integer>
			<key>prtMarkerSuppliesType</key>
			<integer>21</integer>
		</dict>
	</array>
</dict>
</plist>
PLIST
}

case "$1" in
    levels-45) supply 45 100 ;;
    scaled) supply 300 1200 ;;
    unknown-capacity) supply 80 -2 ;;
    some-remaining) supply -3 100 ;;
    slow) exec sleep 10 ;;
    silent) ;;
    *) exit 1 ;;
esac
