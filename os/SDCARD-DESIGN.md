# SD cards: what it would take

A sketch, not an implementation. Written before the code so the shape can be
argued with cheaply.

## Why it is wanted

Flash on these parts runs from 384 KB to 3 MB, and it is shared with the
firmware. An SD card is gigabytes, removable, and readable on a computer without
the device being involved — which covers logging, captures, media, and moving
data off a device that has no network.

It is also the last hardware capability v1 did not have and v2 does not either.

## What already exists

`fw_spi_*` is in the ABI and works. SPI mode is the only mode worth using here:
the four-bit SDIO mode is faster and needs pins and a peripheral RP2 does not
have, so every microcontroller SD library uses SPI and so should this.

That means **a driver is possible as a package today**, with no firmware change
at all. Whether it should be is the first real decision.

## The decision: package or firmware

**As a package**, `sd` would register commands and expose nothing else. Files on
the card would be reachable through those commands and nowhere else — not to
`ls`, not to `cat`, not to a script, not to another package.

**In the firmware**, the card becomes a second mount and every existing command
works on it unchanged. That is obviously nicer and costs the firmware the whole
of FatFs, which is 20–30 KB — against 83 KB of headroom on a Pico W.

There is a third option that is probably the right one:

**The driver is a package; the FILESYSTEM is firmware.** The card's block layer
(init, CMD17, CMD24, CRC, the card-type dance) lives in a package where it can
be iterated on without reflashing. The FAT layer already exists — `core/fat12.*`
reads and writes FAT volumes today for the USB transfer area — and would need
extending to FAT16 and FAT32 to be useful on a card anyone else has formatted.

That splits the work at the seam where the risk is: the block layer is fiddly
and needs a real card in front of it, the filesystem layer is testable on a
host, and `fat12_test` already synthesises a whole volume and hands it to
`fsck.fat` rather than trusting a reading of the specification.

## The block layer, concretely

Initialisation is the part that goes wrong, and it goes wrong quietly — a card
that half-initialises returns plausible rubbish rather than an error.

1. 74+ clock cycles with CS high, at 100–400 kHz. Cards need this and skipping
   it works on the card you have and not on somebody else's.
2. CMD0 → idle state.
3. CMD8 → decides v1 from v2. A card that rejects CMD8 is v1 and takes a
   different initialisation path.
4. ACMD41 in a loop until it stops returning idle, with a timeout. Cards take
   up to a second.
5. CMD58 → the OCR register, which says whether addressing is by BYTE or by
   BLOCK. Getting this backwards reads the right data from the wrong place by a
   factor of 512 and looks like corruption.
6. Only now raise the clock.

Then CMD17 to read a block and CMD24 to write one, both 512 bytes, both with a
CRC that most cards ignore in SPI mode and some do not.

**Card detect and write protect are pins, not commands.** A card removed
mid-write is the normal case, not the exceptional one, and the driver has to
notice rather than time out.

## What the firmware side needs

- **FAT16 and FAT32.** `core/fat12.cpp` is FAT12 because a 1 MB transfer area
  cannot be anything else — the format is decided by the cluster count. A card
  will be FAT32 unless it is tiny. The directory and cluster-chain logic is
  shared; the table width and the boot sector differ.
- **A second mount.** `storage_*` assumes one volume. A path like `/sd/log.txt`
  has to reach a different block device, which is a routing layer over the
  existing calls rather than a second copy of them.
- **Removal.** Every open handle on a card that is no longer there has to fail
  cleanly. This is the part most likely to be got wrong and the part a host test
  can genuinely cover.

## What would be tested where

| Layer | Where | How |
|---|---|---|
| Command framing, CRC, the init sequence | host | a fake card that answers like the specification says, including the ways real cards deviate |
| FAT16/FAT32 read and write | host | synthesise a volume, hand it to `fsck.fat` |
| Removal mid-operation | host | a block device that starts returning "gone" |
| Timing, real cards, the init dance | device | several cards from different makers |

The last row is the one that cannot be moved, and it is why the block layer
belongs in a package first: reflashing to try a theory about a card is slower
than reinstalling one.

## Order

1. FAT16 and FAT32 in `core/fat12.*`, host-tested against `fsck.fat`. Useful on
   its own — it makes the USB transfer area able to be larger.
2. The `sd` package: block layer, `sd info`, `sd read`, raw block access. Proves
   the card talks.
3. The mount layer, once there is a card that reliably answers.

Nothing in step 1 depends on having hardware, which is the argument for starting
there.
