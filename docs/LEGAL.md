# Legal notice, and the risks that are yours

*Not legal advice. This states the project's position and what is known about
the applicable rules; if a decision matters to you, ask a lawyer.*

## No warranty, no liability

Halyard is distributed under GPL-3.0-or-later, whose **sections 15 and 16** are
part of the licence you receive it under: the program comes with **no warranty
of any kind**, and no copyright holder or contributor is liable for any damage
arising from using or being unable to use it — including loss of data, loss of
service, or a third party's decision to act against you.

That is the legal backbone. The rest of this page is the part that matters in
practice.

## The risk that is yours: your Shadow account

**Using an unofficial client may breach Shadow's terms of service, and Shadow
may suspend or terminate your account for it.** Nobody involved in this project
can prevent that, appeal it for you, or compensate you. If your subscription
matters to you, understand that before you install anything.

This is not a theoretical caveat added for form. A cloud service can end a
contract on grounds of its own choosing, and "you connected with software we did
not write" is a ground a provider can use. The technical merits of this client
have no bearing on that decision.

**What reduces the risk, and it is not nothing**: this client identifies itself
honestly, uses your own credentials, requests only the resources your own
subscription grants, and adds no traffic the official client does not. It does
not impersonate the official client to evade detection — where it reproduces the
official client's wire format byte for byte, that is because a protocol
implementation has no choice, not to disguise itself.

**What increases it**: running it on a platform Shadow does not support, which
is the entire point of the project. Draw your own conclusion.

## What this project does not do

- **It does not circumvent authentication.** You sign in with your own account,
  through Shadow's own OAuth device-grant flow.
- **It does not circumvent payment or any access control.** It gives you access
  to a machine you already rent. Without a valid subscription it does nothing.
- **It does not break DRM.** The stream is decrypted with the session key the
  server hands the client during the normal handshake — the same one the
  official client receives.
- **It does not redistribute anything of Shadow's.** Their desktop client is not
  in this repository, and never will be; see `THIRD_PARTY_NOTICES.md`.

If you want service you have not paid for, this project is of no use to you and
the maintainers are not interested in helping.

## On reverse engineering

Shadow's terms, like most, contain a clause against reverse engineering. Two
things are worth stating precisely, because they are often conflated.

**Under EU law, a contract cannot remove the interoperability exception.**
Directive 2009/24/EC on the legal protection of computer programs provides that
a lawful user may **observe, study and test** a program's functioning to
determine the ideas and principles behind it (Article 5(3)), and may
**decompile** it where that is indispensable to obtain the information needed to
achieve interoperability with an independently created program (Article 6).
Article 8 then makes **any contractual stipulation contrary to those provisions
null and void**. France transposes this at Article **L122-6-1** of the Code de la
propriété intellectuelle, whose section V states in as many words that a clause
contrary to it is « nulle et non avenue ».

This project is an interoperability case in the ordinary sense: an independently
written client that talks to a service, for a platform that has none. The
findings recorded in `KB.md` are protocol facts — message layouts, port
assignments, framing — not copied code.

**But that answers a copyright question, not a commercial one.** The exception
means a ToS clause cannot, in the EU, make interoperability research unlawful.
It does **not** oblige a provider to keep serving you, and it says nothing about
their right to end a subscription. Those are separate questions with separate
answers, and only the first has a clear one.

Jurisdictions outside the EU differ, sometimes sharply. This section describes
EU and French law because that is where the project and the service both sit.

## Trademarks

Shadow is a trademark of its owner. Nintendo Switch is a trademark of Nintendo.
PlayStation Vita is a trademark of Sony Interactive Entertainment. Those names
appear in this project only to state which service the client speaks to and
which hardware it runs on — a descriptive, nominative use.

**This project is not affiliated with, endorsed by, or sponsored by Shadow,
Nintendo, or Sony Interactive Entertainment.**

## If you are Shadow

Get in touch through the repository's issues. The protocol knowledge here was
obtained by observing a paying customer's own sessions with their own account,
and documented for interoperability. If something in this repository crosses a
line you need drawn differently, say which and why, and it will be discussed
seriously.
