# PowerVR core, headless part (WP2.3)

`runtime/pvr/core.{h,cpp}` is everything of the graphics chip that a game's frame loop needs before
a single pixel is drawn: the register block, the Tile Accelerator's parameter parser with its
list-complete interrupts, render start/done timing, and the texture and YUV upload paths. The
renderer (Vulkan through MoltenVK, ADR 9) comes after the MoltenVK spike and consumes the parameter
stream this part captures. Parsing rules follow Flycast's `ta.cpp` (GPL-2.0, ADR 1).

## Registers (0x005F8000)

ID 0x17FD11DB, REVISION 0x11, SOFTRESET (bit 0 resets the TA state), STARTRENDER (starts a render),
TA_LIST_INIT (bit 31: resets the parser, `TA_NEXT_OPB = TA_NEXT_OPB_INIT`, `TA_ITP_CURRENT =
TA_ISP_BASE`), TA_LIST_CONT, FB_R_CTRL (bit 23 selects the 27 MHz pixel clock, forwarded to the
SPG), FB_R_SOF1 (a write is a frame swap), FB_W_SOF1/2 masks. The SPG registers keep their own
handler. Everything else is stored and read back; palette and fog RAM are plain storage for now.

## TA parser

Data arrives as 32-byte chunks: store-queue bursts to 0x10000000 or word writes assembled in
address order. Each chunk's first word is the parameter control word: type in bits 31:29, list in
26:24, object control in 15:0. The state machine mirrors the hardware:

| Parameter | Effect |
|---|---|
| End of list (0) | list-complete interrupt for the open list (opaque 7, opaque modifier 8, translucent 9, translucent modifier 10, punch-through 21); list closed |
| User tile clip (1), object list set (2) | 32 bytes, no state change |
| Polygon / modifier volume (4) | opens the list if none is open; a modifier-volume list expects 64-byte volumes; a polygon header is 64 bytes for intensity-coloured textured polygons with offset colour or intensity two-volume polygons; vertices are 64 bytes for textured floating-colour or textured two-volume polygons |
| Sprite (5) | 32-byte header, 64-byte vertices |
| Vertex (7) | 32 or 64 bytes as the header decided |

Second halves of 64-byte parameters carry no control word and are consumed blind. Per list the
parser counts polygons, sprites, modifier volumes, vertices, chunks and ends; the raw words since the
last list init are kept in `Ta::stream` for the renderer. `TA_ITP_CURRENT` advances 32 bytes per
chunk so code that watches it sees progress.

## Rendering and presentation

STARTRENDER schedules the three render-done interrupts (TSP 2, ISP 1, video 0) after a modelled
time: 200,000 cycles plus 4 per captured word, adjustable. FB_R_SOF1 writes count as frame swaps. The
texture path (0x11000000) writes into the 64-bit VRAM view; the YUV converter input (0x10800000) is
counted and otherwise dropped until the renderer exists.

## Not done

The renderer itself (MoltenVK spike, OIT, texture decode, render-to-texture, framebuffer
presentation, SDL3 window), the YUV converter, palette and fog tables as data the renderer reads, and
render timing measured against Flycast on the real title. The launcher reports the per-list counts
so the first Crazy Taxi frames can be checked for shape before anything is drawn.
