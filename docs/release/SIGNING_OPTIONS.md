# Windows signing options

Primary sources checked 2026-11-10 UTC. Prices exclude any additional taxes, subscriptions,
shipping and usage charges; recheck before purchase. This updates the separate historical
audit's signing research, including its incomplete Azure price and Certum cost assumptions.
The existing candidate remains unsigned. No account, purchase, agreement or signing took place.

| Option | Observed cost | Identity and key custody |
| --- | --- | --- |
| Azure Artifact Signing Basic | Microsoft Learn lists US$9.99/account/month, 5,000 signatures; US$0.005 each over quota | Public-trust individuals must be in US/Canada; an Individual Azure billing account supplies legal name/address, followed by identity verification. Microsoft manages signing keys and short-lived certificates. |
| Certum Open Source electronic code | Shop lists from EUR25; observed out of stock | Requires an existing compatible cryptoCertum card and reader, which are additional costs. Identity and address evidence plus a publicly available project identifying the developer are required. The open-source terms restrict commercial distribution. |
| SSL.com individual validation | US$129 for one year; optional YubiKey US$379; eSigner subscription additional | Verified personal name, without business registration; approved hardware or cloud HSM custody. Check the full identity and delivery requirements directly with the issuer. |

Sources: [Azure pricing table](https://learn.microsoft.com/en-us/azure/artifact-signing/how-to-change-sku),
[identity requirements](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart),
[managed certificates](https://learn.microsoft.com/en-us/azure/artifact-signing/concept-certificate-management),
[Certum product](https://shop.certum.eu/open-source-code-signing-code.html),
[Certum verification and restrictions](https://support.certum.eu/en/code-signing-required-documents/),
[SSL.com IV pricing and custody](https://www.ssl.com/products/software-integrity/code-signing/iv/).
An attempted Certum cloud-product URL was inaccessible; no cloud quote or eligibility is inferred.
Microsoft's current certificate documentation describes Windows Platform Trust List trust and
72-hour certificates renewed daily; do not repeat the older blanket claim that this service's
CAs remain in the Microsoft Trusted Root Program.

Recommend evaluating Azure Basic first if the owner meets its geographic and billing requirements.
Compare the full annual hardware/cloud cost and issuer terms before choosing an alternative.
Open-source licensing alone does not establish eligibility for Certum's commercial-use restrictions.
The owner's eligibility, identity disclosure preferences and purchase decision remain pending.

For a future separately authorized signing run:

1. Qualify source and build inputs; sign Nova's application/bridge before packaging them.
   Integrate uninstaller signing into NSIS generation before it is embedded, then sign the final
   installer. Signing an uninstaller after embedding it would not update the installer copy.
2. Use SHA-256 file digests and RFC 3161 timestamps (`/fd SHA256`, `/tr` with the chosen service's
   documented TSA, `/td SHA256`). Protect tokens with a PIN and signing accounts with MFA and
   least-privilege roles. Keep private keys out of repositories and release artifacts.
3. Verify with `signtool verify /pa /all /tw /v <file>` and `Get-AuthenticodeSignature`.
   Check signer identity, all signatures, timestamp chain, trust and revocation; fail on warnings
   requiring investigation. For Artifact Signing, use the current supported SignTool integration
   and Microsoft timestamp endpoint from its integration instructions.
4. Hash the final signed bytes, regenerate manifests/checksums, and qualify a newly identified
   candidate, including installer/repair/uninstall and dependency isolation. Do not reuse today's
   unsigned candidate identity or its qualification results for changed artifacts.

References: [SignTool and timestamp parameters](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool),
[Artifact Signing integration](https://learn.microsoft.com/en-us/azure/artifact-signing/how-to-signing-integrations).
Signing establishes publisher identity and integrity; it is not legal/release approval or a
guarantee of warning-free SmartScreen behavior.
