# Security

## Reporting a vulnerability

Please report security issues privately through GitHub: on this repository's **Security** tab, choose **Report a vulnerability**. Don't open a public issue for them.

Include what you found, how to reproduce it (a file, project or steps), and which edition and commit you used. You'll get a reply as soon as a maintainer can look at it. CorvCAD is a volunteer project, so there is no guaranteed response time.

## Scope

In scope: CorvCAD's own code, meaning the app (`app/`), the engine (`engine/`), the integration layers under `integrations/`, and the build and CI scripts. Examples are a crafted KiCad, FreeCAD or Gerber file that crashes the app, corrupts a project or escapes the app's sandbox, or a build script that runs untrusted input.

Issues in KiCad, FreeCAD, Open CASCADE, ngspice or another bundled component belong upstream; please report them to that project. If CorvCAD's patches or the way it calls the component are at fault, report it here.

## Supported versions

There are no releases yet. Fixes land on `main`.
