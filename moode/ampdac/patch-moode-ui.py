#!/usr/bin/env python3
"""Point moOde's Audio page and config tabs at the libampdac switch."""
import pathlib
import sys

WWW = pathlib.Path("/var/www")

PRELUDE = """// ampdac-snd-prelude
$ampdac_handled = false;
$ampdac_on = file_exists('/var/lib/ampdac.route');
$ampdac_name = '';
$ampdac_card = '';
$ampdac_meta = array();
exec('sudo -n /usr/local/sbin/ampdac-moode label 2>/dev/null', $ampdac_meta);
foreach ($ampdac_meta as $ampdac_line) {
	if (strncmp($ampdac_line, 'name=', 5) === 0) {
		$ampdac_name = substr($ampdac_line, 5);
	} else if (strncmp($ampdac_line, 'card=', 5) === 0) {
		$ampdac_card = substr($ampdac_line, 5);
	}
}
if (isset($_POST['update_output_device'])) {
	$ampdac_choice = strval($_POST['output_device_cardnum']);
	$ampdac_route = '';
	if ($ampdac_choice === 'ampdac' && !$ampdac_on) {
		$ampdac_route = 'ampdac';
	} else if ($ampdac_on && $ampdac_card !== '' && $ampdac_choice === strval($ampdac_card)) {
		$ampdac_route = 'kernel';
	} else if ($ampdac_on && $ampdac_choice !== 'ampdac') {
		exec('sudo -n /usr/local/sbin/ampdac-moode release 2>/dev/null');
		$ampdac_on = false;
	}
	if ($ampdac_route !== '') {
		$ampdac_out = array();
		$ampdac_rc = 0;
		exec('sudo -n /usr/local/sbin/ampdac-moode ' . $ampdac_route . ' 2>&1', $ampdac_out, $ampdac_rc);
		$ampdac_meta = array();
		exec('sudo -n /usr/local/sbin/ampdac-moode label 2>/dev/null', $ampdac_meta);
		foreach ($ampdac_meta as $ampdac_line) {
			if (strncmp($ampdac_line, 'name=', 5) === 0) {
				$ampdac_name = substr($ampdac_line, 5);
			} else if (strncmp($ampdac_line, 'card=', 5) === 0) {
				$ampdac_card = substr($ampdac_line, 5);
			}
		}
		if ($ampdac_name !== '') {
			phpSession('write', 'adevname', $ampdac_name);
			sqlUpdate('cfg_system', $dbh, 'adevname', $ampdac_name);
		}
		if ($ampdac_card !== '') {
			phpSession('write', 'cardnum', $ampdac_card);
			sqlUpdate('cfg_system', $dbh, 'cardnum', $ampdac_card);
			sqlUpdate('cfg_mpd', $dbh, 'device', $ampdac_card);
		}
		$_SESSION['notify']['title'] = 'Output device';
		$_SESSION['notify']['msg'] = ($ampdac_rc === 0) ? 'Switched' : 'Could not switch the output';
		$_SESSION['notify']['duration'] = 2;
		$ampdac_handled = true;
		$ampdac_on = file_exists('/var/lib/ampdac.route');
	}
}
// /ampdac-snd-prelude
"""

LABEL = """\t\t// ampdac-snd-label
\t\tif ($ampdac_name !== '' && $deviceNames[$i] == ALSA_EMPTY_CARD && strval($i) !== strval($ampdac_card)) {
\t\t\tcontinue;
\t\t}
\t\tif ($ampdac_card !== '' && strval($i) === strval($ampdac_card) && $ampdac_name !== '') {
\t\t\t$deviceName = $i . ': ' . $ampdac_name;
\t\t}
\t\t$ampdac_pick = (strval($cfgMPD['device']) === strval($i));
\t\tif ($ampdac_on && $ampdac_card !== '' && strval($i) === strval($ampdac_card)) {
\t\t\t$ampdac_pick = false;
\t\t}
\t\t// /ampdac-snd-label
"""

OPTION = """\t// ampdac-snd-option
\tif ($ampdac_name !== '') {
\t\t$_select['device'] .= "<option value=\\"ampdac\\" " . ($ampdac_on ? "selected" : "") . ">libampdac · " . $ampdac_name . "</option>\\n";
\t}
\t// /ampdac-snd-option
"""

STATUS = """// ampdac-snd-status
if ($ampdac_on) {
	$ampdac_route_label = 'Playing through libampdac'
		. ($ampdac_name !== '' ? ' · ' . $ampdac_name : '')
		. '. The other entry is the same DAC on the kernel driver. Switching during playback reopens the output; a short gap is the USB handoff.';
} else {
	$ampdac_route_label = 'Playing through the kernel driver'
		. ($ampdac_name !== '' ? ' · ' . $ampdac_name : '')
		. '. libampdac in this list is the same DAC. Switching during playback reopens the output; a short gap is the USB handoff.';
}
// /ampdac-snd-status
"""

ROUTE = """\t\t\t<!-- ampdac-snd -->
\t\t\t<span class="help-block">$ampdac_route_label <a href="ampdac-config.php">ampdac</a></span>
\t\t\t<!-- /ampdac-snd -->
"""

TAB = """\t\t\t<!-- ampdac-tab --><a id="ampdac-config-btn" class="btn" href="ampdac-config.php"><span>ampdac</span><i class="fa-solid fa-sharp fa-usb"></i></a><!-- /ampdac-tab -->
"""


def cut(text, start_mark, end_mark):
    start = text.find(start_mark)
    if start < 0:
        return text, False
    end = text.find(end_mark, start)
    if end < 0:
        raise SystemExit(f"ampdac: маркер {start_mark} без конца")
    end += len(end_mark)
    if text[end:end + 1] == "\n":
        end += 1
    return text[:start] + text[end:], True


def insert_once(text, marker, anchor, payload, after=True):
    if marker in text:
        return text
    at = text.find(anchor)
    if at < 0:
        print(f"ampdac: не найдено место для {marker}")
        return text
    if after:
        at += len(anchor)
        if text[at:at + 1] == "\n":
            at += 1
    return text[:at] + payload + text[at:]


def install():
    snd = WWW / "snd-config.php"
    text = snd.read_text(encoding="utf-8")
    for start, end in (
        ("// ampdac-snd-prelude\n", "// /ampdac-snd-prelude\n"),
        ("\t\t// ampdac-snd-label\n", "\t\t// /ampdac-snd-label\n"),
        ("// ampdac-snd-status\n", "// /ampdac-snd-status\n"),
        ("\t// ampdac-snd-option\n", "\t// /ampdac-snd-option\n"),
    ):
        text, _ = cut(text, start, end)
    stock = "(($cfgMPD['device'] == $i) ? \"selected\" : \"\")"
    picked = "($ampdac_pick ? \"selected\" : \"\")"
    if stock in text:
        text = text.replace(stock, picked, 1)
    elif picked not in text:
        raise SystemExit("ampdac: option line in snd-config.php was not found")
    option_anchor = "\t\t$_select['device'] .= \"<option value=\\\"\" . $i . \"\\\" \" . ($ampdac_pick ? \"selected\" : \"\") . \">$deviceName</option>\\n\";\n"
    text = insert_once(text, "ampdac-snd-label", option_anchor, LABEL, after=False)
    # The for-loop closes on the line after that option.
    loop_close = option_anchor + "\t}\n"
    if loop_close not in text:
        raise SystemExit("ampdac: output-device loop close was not found")
    text = text.replace(loop_close, option_anchor + "\t}\n" + OPTION, 1)
    text = insert_once(text, "ampdac-snd-prelude", "// Output device\n", PRELUDE + "\n", after=False)
    handled = "if (empty($ampdac_handled) && isset($_POST['update_output_device']) && $_POST['output_device_cardnum'] != $_SESSION['cardnum']) {"
    plain = "if (isset($_POST['update_output_device']) && $_POST['output_device_cardnum'] != $_SESSION['cardnum']) {"
    if handled not in text:
        if plain not in text:
            raise SystemExit("ampdac: output device handler was not found")
        text = text.replace(plain, handled, 1)
    text = insert_once(text, "ampdac-snd-status", "// Volume type\n", STATUS + "\n", after=False)
    snd.write_text(text, encoding="utf-8")

    tpl = WWW / "templates" / "snd-config.html"
    html = tpl.read_text(encoding="utf-8")
    if "ampdac-snd" not in html:
        needle = "The SCAN button refreshes the list to pick up newly connected USB audio devices."
        at = html.find(needle)
        if at < 0:
            print("ampdac: не найден текст Output device")
        else:
            end = html.find("</span>", at)
            if end < 0:
                print("ampdac: не закрыт блок справки Output device")
            else:
                end = html.find("\n", end)
                end = len(html) if end < 0 else end + 1
                html = html[:end] + ROUTE + html[end:]
    tpl.write_text(html, encoding="utf-8")

    header = WWW / "header.php"
    hdr = header.read_text(encoding="utf-8")
    hdr = insert_once(
        hdr,
        "ampdac-tab",
        '<a id="snd-config-btn" class="btn" href="snd-config.php"><span>Audio</span><i class="fa-solid fa-sharp fa-volume-up"></i></a>\n',
        TAB,
        after=True,
    )
    header.write_text(hdr, encoding="utf-8")


def remove():
    snd = WWW / "snd-config.php"
    if snd.exists():
        text = snd.read_text(encoding="utf-8")
        text, _ = cut(text, "// ampdac-snd-prelude\n", "// /ampdac-snd-prelude\n")
        text, _ = cut(text, "\t\t// ampdac-snd-label\n", "\t\t// /ampdac-snd-label\n")
        text, _ = cut(text, "// ampdac-snd-status\n", "// /ampdac-snd-status\n")
        text, _ = cut(text, "\t// ampdac-snd-option\n", "\t// /ampdac-snd-option\n")
        text = text.replace(
            "($ampdac_pick ? \"selected\" : \"\")",
            "(($cfgMPD['device'] == $i) ? \"selected\" : \"\")",
            1,
        )
        text = text.replace(
            "if (empty($ampdac_handled) && isset($_POST['update_output_device']) && $_POST['output_device_cardnum'] != $_SESSION['cardnum']) {",
            "if (isset($_POST['update_output_device']) && $_POST['output_device_cardnum'] != $_SESSION['cardnum']) {",
            1,
        )
        snd.write_text(text, encoding="utf-8")
    tpl = WWW / "templates" / "snd-config.html"
    if tpl.exists():
        text = tpl.read_text(encoding="utf-8")
        text, _ = cut(text, "\t\t\t<!-- ampdac-snd -->\n", "\t\t\t<!-- /ampdac-snd -->\n")
        tpl.write_text(text, encoding="utf-8")
    header = WWW / "header.php"
    if header.exists():
        text = header.read_text(encoding="utf-8")
        text, _ = cut(text, "\t\t\t<!-- ampdac-tab -->", "<!-- /ampdac-tab -->\n")
        header.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ("install", "remove"):
        raise SystemExit("usage: patch-moode-ui.py install|remove")
    install() if sys.argv[1] == "install" else remove()
