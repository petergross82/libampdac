'use strict';

var fs = require('fs');
var exec = require('child_process').exec;
var execSync = require('child_process').execSync;
var libQ = null;
try {
    libQ = require('/volumio/node_modules/kew');
} catch (e) {
    libQ = null;
}

var AMPDAC_CARD = {
    id: 'ampdac',
    alsacard: 'ampdac',
    name: 'libampdac'
};

// Volumio compares the promise with kew via Object.prototype.toString.
// A native Promise is "[object Promise]" and is rejected as non-compliant.
function pending() {
    var state = 0;
    var value;
    var ok = [];
    var bad = [];

    function flush() {
        if (state === 0) {
            return;
        }
        var list = state === 1 ? ok : bad;
        var queued = list.splice(0);
        ok = [];
        bad = [];
        queued.forEach(function (fn) {
            fn(value);
        });
    }

    function settle(next, result) {
        if (state !== 0) {
            return;
        }
        state = next;
        value = result;
        flush();
    }

    var promise = {
        then: function (onOk, onBad) {
            var next = pending();
            function run(fn, arg, fallback) {
                try {
                    var out = fn ? fn(arg) : fallback(arg);
                    if (out && typeof out.then === 'function') {
                        out.then(next.resolve, next.reject);
                    } else {
                        next.resolve(out);
                    }
                } catch (err) {
                    next.reject(err);
                }
            }
            ok.push(function (arg) { run(onOk, arg, function (v) { return v; }); });
            bad.push(function (arg) { run(onBad, arg, function (err) { throw err; }); });
            flush();
            return next.promise;
        },
        fail: function (onBad) {
            return this.then(null, onBad);
        }
    };

    return {
        promise: promise,
        resolve: function (v) { settle(1, v); },
        reject: function (e) { settle(2, e); }
    };
}

function resolved(value) {
    var d = pending();
    d.resolve(value);
    return d.promise;
}

function ControllerAmpdac(context) {
    this.context = context;
    this.commandRouter = this.context.coreCommand;
    this.logger = this.context.logger;
}

function runRoot(script, logger, soft) {
    var d = libQ ? libQ.defer() : pending();
    exec('/usr/bin/sudo -n ' + script, function (err, stdout, stderr) {
        var text = ((stdout || '') + '\n' + (stderr || '')).trim();
        if (text && logger) {
            logger.info(text);
        }
        if (err) {
            if (logger) {
                logger.error(err.message);
            }
            // Uninstall runs uninstall.sh as root via sudo -S. A failed
            // sudo -n must not reject onStop, or Volumio never gets that far.
            if (soft) {
                d.resolve();
            } else {
                d.reject(err);
            }
            return;
        }
        d.resolve();
    });
    return d.promise;
}

function alsaController(self) {
    try {
        return self.commandRouter.pluginManager.getPlugin('audio_interface', 'alsa_controller');
    } catch (e) {
        return null;
    }
}

function ampdacSelected(alsa) {
    try {
        return alsa.config.get('outputdevicecardname') === 'ampdac' ||
            String(alsa.config.get('outputdevice')) === 'ampdac';
    } catch (e) {
        return false;
    }
}

function setUsbIgnore(alsa, on) {
    try {
        alsa.ignoreUsbAudioDetach(on);
        alsa.ignoreUsbAudioAttach(on);
    } catch (e) {}
}

// Playback Options lists aplay -l cards. The plugin PCM is not a card, so
// the web kept showing only the kernel DAC and MPD kept opening hw: via
// pcm.volumio. Inject one card while this plugin is on.
function installHooks(self) {
    var alsa = alsaController(self);
    // device "volumio" exists only on this path. Without it, a fake card
    // would be opened as hw:ampdac and playback would fail.
    if (process.env.MODULAR_ALSA_PIPELINE !== 'true') {
        if (self.logger) {
            self.logger.error('ampdac: у этой Volumio нет modular ALSA, второе устройство не добавлено');
        }
        return true;
    }
    if (!alsa || typeof alsa.getAlsaCards !== 'function' ||
            typeof alsa.internalUpdateALSAConfigFile !== 'function') {
        return false;
    }
    if (alsa._ampdacHooked) {
        return true;
    }

    alsa._ampdacOrigGetAlsaCards = alsa.getAlsaCards;
    alsa._ampdacOrigUpdate = alsa.internalUpdateALSAConfigFile;
    alsa._ampdacOrigUpdateFile = alsa.updateALSAConfigFile;
    alsa._ampdacOrigSetMixer = alsa.setDefaultMixer;
    alsa._ampdacOrigSave = alsa.saveAlsaOptions;

    if (typeof alsa.updateALSAConfigFile === 'function') {
        // The stock wrapper always sets alsa.outputdevice. MPD's callback
        // treats that as a new device and restarts itself, dropping the queue.
        alsa.updateALSAConfigFile = function () {
            var self = this;
            function run() {
                return self.internalUpdateALSAConfigFile();
            }
            var prev = self.pendingALSAConfigUpdate;
            var promise = prev && typeof prev.then === 'function' ? prev.then(run, run) : run();
            self.pendingALSAConfigUpdate = promise;
            return promise;
        };
    }

    if (typeof alsa.saveAlsaOptions === 'function') {
        alsa.saveAlsaOptions = function (data) {
            var state = null;
            try {
                state = this.commandRouter.volumioGetState();
            } catch (e) {}
            this._ampdacWasPlaying = !!(state && state.status === 'play');
            var incoming = data && data.output_device;
            var incomingId = incoming && (incoming.value !== undefined ? incoming.value : incoming.id);
            var previousId = '';
            try {
                previousId = String(this.config.get('outputdevice') || '');
            } catch (e) {}
            this._ampdacUserSwitch = String(incomingId) !== previousId;
            if (String(incomingId) === 'ampdac') {
                rememberCurrent(alsa);
            }
            return alsa._ampdacOrigSave.apply(this, arguments);
        };
    }

    alsa.getAlsaCards = function () {
        var cards = alsa._ampdacOrigGetAlsaCards.call(this);
        var i;
        var live = null;
        var hasAmp = false;
        if (!cards) {
            cards = [];
        }
        for (i = 0; i < cards.length; i++) {
            if (!cards[i]) {
                continue;
            }
            if (String(cards[i].id) === 'ampdac') {
                hasAmp = true;
                continue;
            }
            if (!live && !builtinCard(cards[i])) {
                live = cards[i];
            }
        }
        var remembered = rememberedCard();
        if (live) {
            noteLiveDac(this, live);
        } else if (remembered && !listHasCard(cards, remembered)) {
            cards.push(remembered);
        }
        var dacName = (live && live.name) || (remembered && remembered.name) || 'USB DAC';
        var pretty = 'libampdac · ' + dacName;
        rememberAmpdacLabel(this, pretty);
        if (hasAmp) {
            for (i = 0; i < cards.length; i++) {
                if (cards[i] && String(cards[i].id) === 'ampdac') {
                    cards[i].name = pretty;
                    cards[i].alsacard = AMPDAC_CARD.alsacard;
                }
            }
        } else {
            cards.push({
                id: AMPDAC_CARD.id,
                alsacard: AMPDAC_CARD.alsacard,
                name: pretty
            });
        }
        return cards;
    };

    alsa.setDefaultMixer = function (device) {
        if (String(device) !== 'ampdac') {
            return alsa._ampdacOrigSetMixer.call(this, device);
        }
        this.mixertype = 'None';
        try {
            this.commandRouter.sharedVars.set('alsa.outputdevicemixer', 'None');
        } catch (e) {}
        if (this.config.has('mixer_type') == false) {
            this.config.addConfigValue('mixer_type', 'string', 'None');
        } else {
            this.setConfigParam({key: 'mixer_type', value: 'None'});
        }
        if (this.config.has('mixer') == false) {
            this.config.addConfigValue('mixer', 'string', '');
        } else {
            this.setConfigParam({key: 'mixer', value: ''});
        }
        try {
            // Read the kernel mixer before Volumio's switch to mixer None
            // writes 100% into it. live-off puts this level back.
            execSync('/usr/bin/sudo -n /usr/local/sbin/ampdac-asound remember-volume', {
                timeout: 3000,
                encoding: 'utf8'
            });
        } catch (e) {}
        try {
            this.setDeviceVolumeOverride({
                card: 'ampdac',
                pluginType: 'miscellanea',
                pluginName: 'ampdac',
                overrideMixerType: 'None',
                overrideAvoidSoftwareMixer: true
            });
        } catch (e) {
            try {
                this.updateVolumeSettings();
            } catch (e2) {}
        }
    };

    alsa.internalUpdateALSAConfigFile = function () {
        var alsaSelf = this;
        var use = ampdacSelected(alsaSelf);
        var wasPlaying = !!alsaSelf._ampdacWasPlaying;
        var userSwitch = !!alsaSelf._ampdacUserSwitch;
        alsaSelf._ampdacUserSwitch = false;
        alsaSelf._ampdacWasPlaying = false;
        var desired = use ? 'ampdac' : 'kernel';
        var switched = alsaSelf._ampdacApplied !== undefined && alsaSelf._ampdacApplied !== desired;
        setUsbIgnore(alsaSelf, use);
        var d = libQ ? libQ.defer() : pending();
        // live-on writes the ampdac route and sets the flag before MPD
        // reopens. live-off clears the flag before the stock writer, so
        // the path unit does not put the plugin route back. Neither mode
        // restarts mpd or reloads systemd.
        exec('/usr/bin/sudo -n /usr/local/sbin/ampdac-asound ' + (use ? 'live-on' : 'live-off'),
            function (err, stdout, stderr) {
                var text = ((stdout || '') + '\n' + (stderr || '')).trim();
                if (text && self.logger) {
                    self.logger.info('ampdac: asound ' + text);
                }
                if (err && self.logger) {
                    self.logger.error(err.message);
                }
                function reopen() {
                    exec('/usr/bin/sudo -n /usr/local/sbin/ampdac-asound reopen',
                        function (reopenErr, reopenOut, reopenErrText) {
                            var reopenText = ((reopenOut || '') + '\n' + (reopenErrText || '')).trim();
                            if (reopenText && self.logger) {
                                self.logger.info('ampdac: reopen ' + reopenText);
                            }
                            if (reopenErr && self.logger) {
                                self.logger.error(reopenErr.message);
                            }
                            if (wasPlaying) {
                                resumePlayback(alsaSelf);
                            }
                            // changed:true makes Volumio set alsa.outputdevice and
                            // that callback restarts MPD, which drops the queue.
                            d.resolve({changed: false});
                        });
                }
                function finish(wrote) {
                    alsaSelf._ampdacApplied = desired;
                    // Reopen when the user picked the other output or the
                    // route file itself changed. Applying the already saved
                    // route at startup leaves a playing MPD alone.
                    if (!(wrote || switched || userSwitch)) {
                        d.resolve({changed: false});
                        return;
                    }
                    reopen();
                }
                if (use) {
                    var wrote = text.indexOf('unchanged') !== 0 && text.indexOf('changed') === 0;
                    finish(wrote);
                    return;
                }
                var orig = null;
                try {
                    orig = alsa._ampdacOrigUpdate.call(alsaSelf);
                } catch (e) {
                    if (self.logger) {
                        self.logger.error(e.message || e);
                    }
                    reopen();
                    return;
                }
                if (orig && typeof orig.then === 'function') {
                    orig.then(function () { finish(userSwitch || switched); }, function () { finish(userSwitch || switched); });
                } else {
                    finish(userSwitch || switched);
                }
            });
        return d.promise;
    };

    alsa._ampdacHooked = true;
    setUsbIgnore(alsa, ampdacSelected(alsa));
    try {
        var seeded = alsa._ampdacOrigGetAlsaCards.call(alsa) || [];
        var si;
        for (si = 0; si < seeded.length; si++) {
            if (seeded[si] && String(seeded[si].id) !== 'ampdac' && !builtinCard(seeded[si])) {
                noteLiveDac(alsa, seeded[si]);
                break;
            }
        }
    } catch (e) {}
    if (ampdacSelected(alsa)) {
        writeAmpdacAsound(self.logger, 'sync');
    } else {
        rememberCurrent(alsa);
    }
    if (self.logger) {
        self.logger.info('ampdac: в списке выходов есть ядро и libampdac');
    }
    return true;
}

function writeAmpdacAsound(logger, mode) {
    var d = libQ ? libQ.defer() : pending();
    exec('/usr/bin/sudo -n /usr/local/sbin/ampdac-asound ' + (mode || 'sync'), function (err, stdout, stderr) {
        var text = ((stdout || '') + '\n' + (stderr || '')).trim();
        if (text && logger) {
            logger.info('ampdac: asound ' + text);
        }
        if (err) {
            if (logger) {
                logger.error(err.message);
            }
            d.resolve({changed: false});
            return;
        }
        d.resolve({changed: text.indexOf('unchanged') !== 0});
    });
    return d.promise;
}

function removeHooks(alsa) {
    if (!alsa || !alsa._ampdacHooked) {
        return;
    }
    if (alsa._ampdacOrigGetAlsaCards) {
        alsa.getAlsaCards = alsa._ampdacOrigGetAlsaCards;
    }
    if (alsa._ampdacOrigUpdate) {
        alsa.internalUpdateALSAConfigFile = alsa._ampdacOrigUpdate;
    }
    if (alsa._ampdacOrigUpdateFile) {
        alsa.updateALSAConfigFile = alsa._ampdacOrigUpdateFile;
    }
    if (alsa._ampdacOrigSetMixer) {
        alsa.setDefaultMixer = alsa._ampdacOrigSetMixer;
    }
    if (alsa._ampdacOrigSave) {
        alsa.saveAlsaOptions = alsa._ampdacOrigSave;
    }
    alsa._ampdacHooked = false;
}

function firstRealCard(alsa) {
    var cards = [];
    var i;
    try {
        cards = alsa._ampdacOrigGetAlsaCards.call(alsa) || [];
    } catch (e) {
        return null;
    }
    for (i = 0; i < cards.length; i++) {
        if (cards[i] && cards[i].id !== undefined && cards[i].id !== '' &&
                String(cards[i].id) !== 'ampdac') {
            return cards[i];
        }
    }
    return null;
}

function scheduleHooks(self) {
    var left = 15;
    if (self._ampdacHookTimer) {
        clearTimeout(self._ampdacHookTimer);
        self._ampdacHookTimer = null;
    }
    function attempt() {
        if (installHooks(self)) {
            return;
        }
        left -= 1;
        if (left > 0) {
            self._ampdacHookTimer = setTimeout(attempt, 1000);
        } else if (self.logger) {
            self.logger.error('ampdac: alsa_controller не найден, второго выхода не будет');
        }
    }
    attempt();
}

// Volumio does `conf += callback()`. If this plugin is already stopped,
// executeOnPlugin returns undefined and the file gains a line "undefined".
// MPD then refuses to start. Never register that callback, and drop one
// left by an older build before any rewrite of mpd.conf.
function unregisterMpdOutput(self) {
    var mpd = null;
    try {
        mpd = self.commandRouter.pluginManager.getPlugin('music_service', 'mpd');
    } catch (e) {
        return;
    }
    if (!mpd || !mpd.registeredCallbacks) {
        return;
    }
    var kept = [];
    var i;
    for (i = 0; i < mpd.registeredCallbacks.length; i++) {
        var cb = mpd.registeredCallbacks[i];
        if (cb && cb.plugin === 'ampdac') {
            continue;
        }
        kept.push(cb);
    }
    mpd.registeredCallbacks = kept;
}

function shellArg(value) {
    return "'" + String(value == null ? '' : value).replace(/'/g, "'\\''") + "'";
}

function builtinCard(card) {
    var label = String(card.name || '') + ' ' + String(card.id || '') + ' ' + String(card.alsacard || '');
    return /bcm2835|hdmi|headphone/i.test(label);
}

function listHasCard(cards, remembered) {
    var i;
    for (i = 0; i < cards.length; i++) {
        var c = cards[i];
        if (!c) {
            continue;
        }
        if (String(c.id) === String(remembered.id) ||
                (remembered.alsacard && String(c.alsacard) === String(remembered.alsacard))) {
            return true;
        }
    }
    return false;
}

function rememberAmpdacLabel(alsa, pretty) {
    if (!ampdacSelected(alsa) || alsa._ampdacLabel === pretty) {
        return;
    }
    var current = '';
    try {
        current = String(alsa.config.get('outputdevicename') || '');
    } catch (e) {}
    if (current === pretty) {
        alsa._ampdacLabel = pretty;
        return;
    }
    alsa._ampdacLabel = pretty;
    try {
        alsa.setConfigParam({key: 'outputdevicename', value: pretty});
    } catch (e) {}
}

function noteLiveDac(alsa, live) {
    var key = String(live.id) + '\t' + String(live.name || '') + '\t' + String(live.alsacard || '');
    if (alsa._ampdacNoted === key) {
        return;
    }
    alsa._ampdacNoted = key;
    exec('/usr/bin/sudo -n /usr/local/sbin/ampdac-asound remember ' +
        shellArg(live.id) + ' ' + shellArg(live.name || '') + ' ' + shellArg(live.alsacard || ''));
}

function resumePlayback(alsa) {
    try {
        if (alsa.commandRouter && typeof alsa.commandRouter.volumioPlay === 'function') {
            alsa.commandRouter.volumioPlay();
            return;
        }
    } catch (e) {}
    exec('/usr/bin/mpc -q play');
}

function escapeHtml(text) {
    return String(text)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;');
}

function rememberCurrent(alsa) {
    var id;
    var name;
    var card;
    try {
        id = alsa.config.get('outputdevice');
        name = alsa.config.get('outputdevicename');
        card = alsa.config.get('outputdevicecardname');
    } catch (e) {
        return;
    }
    if (id === undefined || id === null || String(id) === '' ||
            String(id) === 'ampdac' || String(card) === 'ampdac') {
        return;
    }
    if (builtinCard({id: id, name: name, alsacard: card})) {
        return;
    }
    exec('/usr/bin/sudo -n /usr/local/sbin/ampdac-asound remember ' +
        shellArg(id) + ' ' + shellArg(name || '') + ' ' + shellArg(card || ''));
}

function rememberedCard() {
    var text;
    try {
        text = fs.readFileSync('/var/lib/ampdac.previous', 'utf8');
    } catch (e) {
        return null;
    }
    var parts = String(text).replace(/\n$/, '').split('\t');
    if (!parts[0] || parts[0] === 'ampdac') {
        return null;
    }
    return {
        id: parts[0],
        name: parts[1] || parts[0],
        alsacard: parts[2] || parts[0]
    };
}

function preferRealCard(alsa) {
    var cards = [];
    var i;
    var fallback = null;
    try {
        cards = alsa._ampdacOrigGetAlsaCards.call(alsa) || [];
    } catch (e) {
        return null;
    }
    for (i = 0; i < cards.length; i++) {
        var c = cards[i];
        if (!c || c.id === undefined || c.id === '' || String(c.id) === 'ampdac') {
            continue;
        }
        var label = String(c.name || '') + ' ' + String(c.id || '') + ' ' + String(c.alsacard || '');
        if (/bcm2835|hdmi|headphone/i.test(label)) {
            if (!fallback) {
                fallback = c;
            }
            continue;
        }
        return c;
    }
    return fallback;
}

ControllerAmpdac.prototype.onVolumioStart = function () {
    var self = this;
    unregisterMpdOutput(self);
    scheduleHooks(self);
    return libQ ? libQ.resolve() : resolved();
};

ControllerAmpdac.prototype.onStart = function () {
    var self = this;
    unregisterMpdOutput(self);
    scheduleHooks(self);
    var started = libQ ? libQ.defer() : pending();
    function startedOk() { started.resolve(); }
    runRoot('/usr/local/sbin/ampdac-patch-mpd', self.logger, true)
        .then(function () {
            return runRoot('/usr/local/sbin/ampdac-asound sync', self.logger, true);
        })
        .then(startedOk, startedOk);
    return started.promise;
};

ControllerAmpdac.prototype.onStop = function () {
    var self = this;
    var alsa = alsaController(self);
    var next = null;
    var switching = false;
    if (self._ampdacHookTimer) {
        clearTimeout(self._ampdacHookTimer);
        self._ampdacHookTimer = null;
    }
    unregisterMpdOutput(self);
    if (alsa && ampdacSelected(alsa)) {
        next = rememberedCard() || preferRealCard(alsa) || firstRealCard(alsa);
        switching = true;
    }
    var d = libQ ? libQ.defer() : pending();
    // Clear the route flag before the stock writer restores hw:K7.
    runRoot('/usr/local/sbin/ampdac-asound off', self.logger, true)
        .then(function () { stopAfterOff(self, alsa, next, switching, d); },
            function () { stopAfterOff(self, alsa, next, switching, d); });
    return d.promise;
};

function stopAfterOff(self, alsa, next, switching, d) {
    removeHooks(alsa);
    if (alsa) {
        setUsbIgnore(alsa, false);
    }
    if (!switching || !next || !alsa || typeof alsa.saveAlsaOptions !== 'function') {
        runRoot('/usr/local/sbin/ampdac-unpatch-mpd', self.logger, true)
            .then(function () { d.resolve(); }, function () { d.resolve(); });
        return;
    }
    var saved;
    try {
        saved = alsa.saveAlsaOptions({
            disallowPush: true,
            i2s: false,
            output_device: {
                value: next.id,
                label: next.name,
                alsacard: next.alsacard
            }
        });
    } catch (e) {
        self.logger.error(e.message || e);
        runRoot('/usr/local/sbin/ampdac-unpatch-mpd', self.logger, true)
            .then(function () { d.resolve(); }, function () { d.resolve(); });
        return;
    }
    function repair() {
        // createMPDFile writes the file after its promise resolves.
        // Strip a leftover "undefined" line only once that write has landed.
        setTimeout(function () {
            runRoot('/usr/local/sbin/ampdac-unpatch-mpd', self.logger, true)
                .then(function () { d.resolve(); }, function () { d.resolve(); });
        }, 500);
    }
    if (saved && typeof saved.then === 'function') {
        saved.then(repair, repair);
    } else {
        repair();
    }
};

// Kept so a previously registered callback does not append a second MPD
// output. Volumio plays device "volumio"; the card list selects the route.
ControllerAmpdac.prototype.getMpdOutput = function () {
    return '';
};

ControllerAmpdac.prototype.showTransport = function () {
    var self = this;
    var d = libQ ? libQ.defer() : pending();
    exec('/usr/bin/ampdac show transport', {timeout: 8000, maxBuffer: 256 * 1024},
        function (err, stdout, stderr) {
            var text = String(stdout || '');
            if (stderr) {
                text += (text ? '\n' : '') + String(stderr);
            }
            if (!text.trim()) {
                text = err ? String(err.message || err) : 'ampdac show transport: пусто';
            }
            var html = '<pre>' + escapeHtml(text) + '</pre>';
            try {
                self.commandRouter.broadcastMessage('openModal', {
                    title: 'ampdac',
                    message: html,
                    size: 'lg',
                    buttons: [{
                        name: 'Close',
                        class: 'btn btn-info',
                        emit: 'closeModals',
                        payload: ''
                    }]
                });
            } catch (e) {
                if (self.logger) {
                    self.logger.error(e.message || e);
                }
            }
            d.resolve({});
        });
    return d.promise;
};

ControllerAmpdac.prototype.getUIConfig = function () {
    var d = libQ ? libQ.defer() : pending();
    var uiconf = JSON.parse(fs.readFileSync(__dirname + '/UIConfig.json', 'utf8'));
    d.resolve(uiconf);
    return d.promise;
};

ControllerAmpdac.prototype.getConfigurationFiles = function () {
    return ['config.json'];
};

module.exports = ControllerAmpdac;
