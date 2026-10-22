#!/usr/bin/env python3
"""Add an Info button on the ampdac row of Installed Plugins.

The button calls plugin method showTransport, which opens
`ampdac show transport` in the existing modal. The classic UI and the
Manifest UI each keep a full copy of the app bundle; patch every copy
that still has the installed-plugin row.
"""
import pathlib
import re
import sys

NAME_OLD = '{{plugin.prettyName}} <small ng-if="plugin.version"'
# The installed-plugin markup lives inside a single-quoted JS string.
# Apostrophes in the Angular expressions have to be escaped or the
# whole Manifest bundle fails to parse and the UI stays blank.
NAME_NEW = (
    '{{plugin.prettyName}} '
    '<button ng-if="plugin.name===\\\'ampdac\\\'" '
    'ng-click="pluginManager.ampdacInfo()" type="button" '
    'class="btn btn-info btn-xs">Info</button> '
    '<small ng-if="plugin.version"'
)
METHOD_OLD = (
    '{key:"showPluginSettings",value:function(e){this.$state.go("volumio.plugin",'
    '{pluginName:""+e.category+"-"+e.name,isPluginSettings:!0})}},'
    '{key:"showPluginCategory"'
)
METHOD_NEW = (
    '{key:"showPluginSettings",value:function(e){this.$state.go("volumio.plugin",'
    '{pluginName:""+e.category+"-"+e.name,isPluginSettings:!0})}},'
    '{key:"ampdacInfo",value:function(){this.socketService.emit("callMethod",'
    '{endpoint:"miscellanea/ampdac",method:"showTransport",data:{}})}},'
    '{key:"showPluginCategory"'
)
# Playback Options keeps a Save button. The output list applies on change,
# the same way moOde submits the output device dropdown.
NAME_BROKEN = (
    '{{plugin.prettyName}} '
    '<button ng-if="plugin.name===\'ampdac\'" '
    'ng-click="pluginManager.ampdacInfo()" type="button" '
    'class="btn btn-info btn-xs">Info</button> '
    '<small ng-if="plugin.version"'
)
SELECT_BROKEN = (
    '<ui-select id="{{item.id}}" ng-model="item.value" search-enabled="false" '
    'ng-readonly="true" append-to-body="true" theme="bootstrap" '
    'on-select="item.id===\'output_device\' && pluginComponent.saveSection(section)">'
)
SELECT_OLD = (
    '<ui-select id="{{item.id}}" ng-model="item.value" search-enabled="false" '
    'ng-readonly="true" append-to-body="true" theme="bootstrap">'
)
SELECT_PLAIN = (
    '<ui-select id="{{item.id}}" ng-model="item.value" search-enabled="false" '
    'ng-readonly="true" append-to-body="true" theme="bootstrap" '
    'on-select="item.id===\\\'output_device\\\' && pluginComponent.saveSection(section)">'
)
# Angular 1.5 rejects the comma operator. The parse error aborts ui-select,
# the match stays empty, and the output row collapses to zero height.
SELECT_COMMA = (
    '<ui-select id="{{item.id}}" ng-model="item.value" search-enabled="false" '
    'ng-readonly="true" append-to-body="true" theme="bootstrap" '
    'on-select="item.id===\\\'output_device\\\' && (item.value=$item, pluginComponent.saveSection(section))">'
)
SELECT_NEW = (
    '<ui-select id="{{item.id}}" ng-model="item.value" search-enabled="false" '
    'ng-readonly="true" append-to-body="true" theme="bootstrap" '
    'on-select="item.id===\\\'output_device\\\' && (item.value=$item) && pluginComponent.saveSection(section)">'
)

ROOTS = (
    pathlib.Path('/volumio/http/www4/scripts'),
    pathlib.Path('/volumio/http/www/scripts'),
)
INDEXES = (
    pathlib.Path('/volumio/http/www4/index.html'),
    pathlib.Path('/volumio/http/www/index.html'),
)


def bundles():
    found = []
    for root in ROOTS:
        if not root.is_dir():
            continue
        found.extend(sorted(root.glob('app-*.js')))
    return found


def patch(text):
    changed = False
    if NAME_BROKEN in text:
        text = text.replace(NAME_BROKEN, NAME_NEW)
        changed = True
    elif 'pluginManager.ampdacInfo()' not in text and NAME_OLD in text:
        text = text.replace(NAME_OLD, NAME_NEW, 1)
        changed = True
    if 'method:"showTransport"' not in text and METHOD_OLD in text:
        text = text.replace(METHOD_OLD, METHOD_NEW, 1)
        changed = True
    if SELECT_COMMA in text:
        text = text.replace(SELECT_COMMA, SELECT_NEW)
        changed = True
    elif SELECT_PLAIN in text:
        text = text.replace(SELECT_PLAIN, SELECT_NEW)
        changed = True
    elif SELECT_BROKEN in text:
        text = text.replace(SELECT_BROKEN, SELECT_NEW)
        changed = True
    elif "item.id==='output_device'" not in text and SELECT_OLD in text:
        text = text.replace(SELECT_OLD, SELECT_NEW)
        changed = True
    return text, changed


def unpatch(text):
    changed = False
    if NAME_NEW in text:
        text = text.replace(NAME_NEW, NAME_OLD, 1)
        changed = True
    elif NAME_BROKEN in text:
        text = text.replace(NAME_BROKEN, NAME_OLD, 1)
        changed = True
    if METHOD_NEW in text:
        text = text.replace(METHOD_NEW, METHOD_OLD, 1)
        changed = True
    if SELECT_NEW in text:
        text = text.replace(SELECT_NEW, SELECT_OLD)
        changed = True
    elif SELECT_COMMA in text:
        text = text.replace(SELECT_COMMA, SELECT_OLD)
        changed = True
    elif SELECT_PLAIN in text:
        text = text.replace(SELECT_PLAIN, SELECT_OLD)
        changed = True
    elif SELECT_BROKEN in text:
        text = text.replace(SELECT_BROKEN, SELECT_OLD)
        changed = True
    return text, changed


# Bump this when the patched bundle must be fetched again. A browser
# that already loaded ?ampdac=1 keeps the broken select until the URL changes.
CACHE_MARK = '?ampdac=2'
CACHE_QUERY = re.compile(r'\?ampdac=\d+')


def patch_index(path, remove):
    if not path.is_file():
        return False
    text = path.read_text(encoding='utf-8', errors='surrogateescape')
    if remove:
        new = CACHE_QUERY.sub('', text)
    else:
        new = text
        token = 'scripts/app-'
        start = 0
        while True:
            i = new.find(token, start)
            if i < 0:
                break
            j = new.find('.js', i)
            if j < 0:
                break
            end = j + 3
            while True:
                query = CACHE_QUERY.match(new, end)
                if query is None:
                    break
                new = new[:end] + new[query.end():]
            new = new[:end] + CACHE_MARK + new[end:]
            start = end + len(CACHE_MARK)
    if new == text:
        return False
    path.write_text(new, encoding='utf-8', errors='surrogateescape')
    return True


def main():
    remove = '--remove' in sys.argv
    touched = 0
    skipped = 0
    for path in bundles():
        raw = path.read_text(encoding='utf-8', errors='surrogateescape')
        if 'showPluginSettings' not in raw or 'installed-plugin' not in raw and 'plugin.prettyName' not in raw:
            continue
        new, changed = unpatch(raw) if remove else patch(raw)
        if not changed:
            if (not remove and 'pluginManager.ampdacInfo()' in raw) or (
                    remove and 'pluginManager.ampdacInfo()' not in raw):
                continue
            skipped += 1
            print('ampdac ui: пропуск %s' % path)
            continue
        path.write_text(new, encoding='utf-8', errors='surrogateescape')
        touched += 1
        print('ampdac ui: %s %s' % ('снято' if remove else 'кнопка Info', path.name))
    for index in INDEXES:
        if patch_index(index, remove):
            touched += 1
            print('ampdac ui: %s %s' % ('снято' if remove else 'кэш', index))
    if touched == 0 and skipped == 0:
        print('ampdac ui: бандлы без строки плагина, кнопка не нужна')
    return 0


if __name__ == '__main__':
    sys.exit(main())
