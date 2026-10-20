<?php
/*
 * SPDX-License-Identifier: MIT
 * moOde page: switch pcm._audioout between the detected card and libampdac.
*/

require_once __DIR__ . '/inc/common.php';
require_once __DIR__ . '/inc/session.php';
require_once __DIR__ . '/inc/sql.php';

$dbh = sqlConnect();
phpSession('open');
chkVariables($_POST);

$kernel_label = isset($_SESSION['adevname']) ? $_SESSION['adevname'] : '';
if ($kernel_label == '' || $kernel_label == 'ampdac') {
    $kernel_label = 'ALSA card';
}

if (isset($_POST['save']) && $_POST['save'] == '1') {
    $route = isset($_POST['route']) ? $_POST['route'] : '';
    if ($route === 'kernel' || $route === 'ampdac') {
        $out = array();
        $rc = 0;
        exec('sudo -n /usr/local/sbin/ampdac-moode ' . $route . ' 2>&1', $out, $rc);
        $shown = array();
        foreach ($out as $line) {
            if (strpos($line, 'mixer=') === 0) {
                $mixer = substr($line, 6);
                if ($mixer !== '') {
                    $_SESSION['mpdmixer'] = $mixer;
                    $_SESSION['mpdmixer_local'] = $mixer;
                }
                continue;
            }
            $shown[] = $line;
        }
        $text = trim(implode(' ', $shown));
        $_SESSION['notify']['title'] = 'ampdac';
        if ($rc === 0) {
            $_SESSION['notify']['msg'] = $text !== '' ? $text : 'Output switched';
        } else {
            $_SESSION['notify']['msg'] = $text !== '' ? $text : 'Could not switch the output';
        }
        $_SESSION['notify']['duration'] = 5;
    }
    phpSession('close');
    header('Location: ampdac-config.php');
    exit;
}

$using_ampdac = file_exists('/var/lib/ampdac.route');
$slave = '';
$conf = @file_get_contents('/etc/alsa/conf.d/_audioout.conf');
if ($conf !== false && preg_match('/^slave\.pcm\s+"(.*)"/m', $conf, $m)) {
    $slave = $m[1];
}

$route_options = '<option value="kernel"' . ($using_ampdac ? '' : ' selected') . '>'
    . htmlspecialchars($kernel_label, ENT_QUOTES, 'UTF-8') . '</option>'
    . '<option value="ampdac"' . ($using_ampdac ? ' selected' : '') . '>libampdac</option>';

if ($using_ampdac) {
    $status_line = 'MPD is playing through libampdac. The card in the Audio list stays the same and comes back when it is selected here again.';
} else {
    $status_line = 'MPD is playing through the card as the system sees it.';
}
if ($slave !== '') {
    $status_line .= ' _audioout: ' . $slave . '.';
}

$transport_lines = array();
$transport_rc = 0;
exec('sudo -n /usr/local/sbin/ampdac-moode transport 2>&1', $transport_lines, $transport_rc);
$transport_text = trim(implode("\n", $transport_lines));
if ($transport_text === '') {
    $transport_text = 'Transport report is not available.';
}

waitWorker('ampdac-config');

$section = basename(__FILE__, '.php');
storeBackLink($section, 'ampdac-config.php');

include('header.php');
?>
<div id="container">
	<div class="container">
		<h1 class="ampdac-config">libampdac</h1>
		<p>Output device. MPD still opens _audioout. This page chooses where that output goes: the DAC as the system detects it, or libampdac.</p>
		<form class="form-horizontal" method="post">
			<div class="control-group">
				<label class="control-label" for="route">Device</label>
				<div class="controls">
					<select id="route" name="route" class="select-large">
						<?php echo $route_options; ?>
					</select>
					<span class="help-block"><?php echo htmlspecialchars($status_line, ENT_QUOTES, 'UTF-8'); ?> While libampdac is selected, ALSA loopback, CamillaDSP and the moOde equalizer stay out of this chain. Volume on this path is left unchanged and is controlled on the DAC.</span>
				</div>
			</div>
			<div class="control-group">
				<label class="control-label">Transport</label>
				<div class="controls">
					<span class="help-block" style="white-space:pre-wrap"><?php echo htmlspecialchars($transport_text, ENT_QUOTES, 'UTF-8'); ?></span>
				</div>
			</div>
			<div class="control-group">
				<div class="controls">
					<button class="btn btn-primary btn-medium" type="submit" name="save" value="1">Apply</button>
				</div>
			</div>
		</form>
	</div>
</div>
<script>
jQuery(function () {
    jQuery('#config-tabs .btn').removeClass('active');
    jQuery('#ampdac-config-btn').addClass('active');
});
</script>
<?php
$footer = file_exists(__DIR__ . '/footer.php') ? 'footer.php' : 'footer.min.php';
include($footer);
