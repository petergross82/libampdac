'use strict';

var libQ = require('kew');
var fs = require('fs-extra');
var exec = require('child_process').exec;

function ControllerAmpdac(context) {
    this.context = context;
    this.commandRouter = this.context.coreCommand;
    this.logger = this.context.logger;
}

ControllerAmpdac.prototype.onVolumioStart = function () {
    return libQ.resolve();
};

ControllerAmpdac.prototype.onStart = function () {
    var self = this;
    var defer = libQ.defer();
    exec('/bin/bash ' + __dirname + '/patch-mpd.sh', function (err, stdout, stderr) {
        if (stdout) self.logger.info(stdout.trim());
        if (err) {
            self.logger.error(stderr || err.message);
            defer.reject(err);
            return;
        }
        defer.resolve();
    });
    return defer.promise;
};

ControllerAmpdac.prototype.onStop = function () {
    var defer = libQ.defer();
    exec('/bin/bash ' + __dirname + '/unpatch-mpd.sh', function () {
        defer.resolve();
    });
    return defer.promise;
};

ControllerAmpdac.prototype.getUIConfig = function () {
    var defer = libQ.defer();
    var uiconf = fs.readJsonSync(__dirname + '/UIConfig.json');
    defer.resolve(uiconf);
    return defer.promise;
};

ControllerAmpdac.prototype.getConfigurationFiles = function () {
    return ['config.json'];
};

module.exports = ControllerAmpdac;
