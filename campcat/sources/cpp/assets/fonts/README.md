# Bundled UI fonts

The faces the app ships with, committed so the build never touches the network.
They were prepared once, from the sources below; provenance and licence follow.

## Sources

| output | upstream | wght | subset |
|---|---|---|---|
| `Inter-Regular.ttf` | [rsms/inter](https://github.com/rsms/inter) release v4.1, `Inter-4.1.zip` → `extras/ttf/Inter-Regular.ttf` | 400 | no |
| `Inter-SemiBold.ttf` | same zip → `extras/ttf/Inter-SemiBold.ttf` | 600 | no |
| `JetBrainsMono-Regular.ttf` | google/fonts `ofl/jetbrainsmono/JetBrainsMono[wght].ttf` | 400 | no |
| `NotoSansSC-Common.ttf` | google/fonts `ofl/notosanssc/NotoSansSC[wght].ttf` | 400 | GB2312 1+2, 6763 hanzi |
| `lucide.ttf` | npm [`lucide-static`](https://www.npmjs.com/package/lucide-static) 1.73.0 → `font/lucide.ttf` | -- | no |

The two google/fonts faces come from commit
`bd8f81ddb5c74d5c8897b36ad88b440266245103`, pulled through jsDelivr. Their
variable fonts are instantiated because ImGui ignores variation axes: shipped
as-is, every weight would draw as Regular. The CJK face is additionally subset:
instancing alone still leaves 10.1 MB, while the subset is 2.1 MB.

Inter comes from its own release rather than google/fonts, which ships it only
as a variable font; the release carries static instances, so nothing here is
re-instanced.

`lucide.ttf` is shipped whole (2379 icons). The app bakes only the codepoints
listed in `campcat/sources/cpp/include/app/ui/icons.h`, so the unused glyphs
cost disk, not atlas. Codepoints are per release: replacing this file means
re-reading `font/codepoints.json` from the same `lucide-static` version.

## Checksums

| file | bytes | sha256 |
|---|---|---|
| `Inter-Regular.ttf` | 411640 | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` |
| `Inter-SemiBold.ttf` | 419744 | `78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3` |
| `JetBrainsMono-Regular.ttf` | 115172 | `15e717083a6237478a91f8c7083a030d170d3e5d91718616f82f94806194e75f` |
| `NotoSansSC-Common.ttf` | 2165900 | `a110a49d9e697ce85f2af8346aef1bd7519b66a30b64c542b79d8f3471f31bec` |
| `lucide.ttf` | 999164 | `20213142b8e0c59a2d359be11eadc5c266ce331b682463dafe7b4f2e33432805` |

## Licence

The text faces are SIL Open Font License 1.1, full texts in `OFL/`; the icon
font is ISC, in `Lucide-ISC.txt`.

- Inter -- Copyright 2016 The Inter Project Authors (https://github.com/rsms/inter)
- JetBrainsMono -- Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono)
- NotoSansSC -- Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'
- Lucide -- Copyright 2026 Lucide Icons and Contributors (https://lucide.dev)

OFL-1.1 permits use, modification, subsetting and redistribution inside an
application. It requires the notice and licence to travel with the binaries
(hence `OFL/`), and that modified copies not carry a Reserved Font Name -- the
outputs here are renamed, and none contains `Source`. ISC asks only that the
copyright and permission notice travel with copies.

The proprietary faces this replaced (Arial, Menlo, Hiragino Sans GB, PingFang,
Arial Unicode) are not redistributable and are deliberately not in this tree.
