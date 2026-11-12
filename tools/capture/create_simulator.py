#!/usr/bin/env python3
"""Create a dedicated 1x iPad capture simulator using the installed Xcode profile.

The logical 834 x 1210 iPad canvas is retained; the rendered framebuffer is 1x.
The copied Apple profile stays on this Mac and is never part of a release artifact.
"""
import argparse
import plistlib
import shutil
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', default='CorvCAD Capture iPad 1x')
    parser.add_argument('--runtime', default='com.apple.CoreSimulator.SimRuntime.iOS-26-2')
    args = parser.parse_args()
    source = Path('/Library/Developer/CoreSimulator/Profiles/DeviceTypes/iPad Pro 11-inch (M4).simdevicetype')
    target = Path.home() / 'Library/Developer/CoreSimulator/Profiles/DeviceTypes/CorvCAD Capture iPad 1x.simdevicetype'
    identifier = 'org.corvcad.simulator.ipad-1x'
    expected = {'mainScreenWidth': 834, 'mainScreenHeight': 1210, 'mainScreenScale': 1,
                'mainScreenWidthDPI': 132, 'mainScreenHeightDPI': 132}
    if target.exists():
        info = plistlib.loads((target / 'Contents/Info.plist').read_bytes())
        profile = plistlib.loads((target / 'Contents/Resources/profile.plist').read_bytes())
        if info.get('CFBundleIdentifier') != identifier or any(profile.get(k) != v for k, v in expected.items()):
            raise RuntimeError('Existing capture profile differs; it has been left untouched.')
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, target)
        path = target / 'Contents/Info.plist'
        info = plistlib.loads(path.read_bytes())
        info['CFBundleIdentifier'] = identifier
        info['CFBundleName'] = 'CorvCAD Capture iPad 1x'
        path.write_bytes(plistlib.dumps(info))
        path = target / 'Contents/Resources/profile.plist'
        profile = plistlib.loads(path.read_bytes())
        profile.update(expected)
        path.write_bytes(plistlib.dumps(profile))
    result = subprocess.check_output(['xcrun', 'simctl', 'create', args.name, identifier, args.runtime], text=True)
    print(result.strip())


if __name__ == '__main__':
    main()
