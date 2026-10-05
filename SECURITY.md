# Security Policy

## Reporting a Vulnerability

If you find a security issue in Voland, please **do not open a public GitHub issue**. Public disclosure before a fix gives bad actors a window to exploit the vulnerability.

Instead open a private security advisory through GitHub: go to the repo's Security tab → Advisories → Report a vulnerability

We will acknowledge receipt within 7 days and provide an estimated timeline for a fix or response within 14 days.

## Scope

Voland is pre-alpha, but it runs untrusted guest code (games and homebrew) in the browser, so these are in scope:

- Memory safety issues in the core or in JIT-compiled output
- Guest code escaping the emulator (reaching the page, other origins, or the host)
- Issues in dependency handling, build tooling or the CI/deploy workflows
- Anything that could compromise users running Voland builds, including the hosted web app

## Out of Scope

- Issues in upstream dependencies (report to the relevant upstream)
- Theoretical issues with the project's design that haven't been implemented yet
- Issues in the Switch 2 console itself or Nintendo's software

## Supported Versions

Pre-alpha. There are no released versions to support yet. Once Voland has releases, this section will list which versions receive security updates.
