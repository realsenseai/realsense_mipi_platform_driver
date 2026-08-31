# GMSL virtual-channel remapping (MAX96712)

Lets a device tree state, per GMSL link, which deserializer CSI virtual channel
each serializer virtual channel comes out on. Before this, the mapping was
hardcoded as `dser_vc = link * 4 + ser_vc`, and the link itself was derived from
the camera's `vc-id` — so VC assignment and link topology could not be chosen
independently.

Applies to **max96712 only**. max9296 and max96724 are currently single-link in practice
and keep using the value d4xx passes them unchanged (TODO: max96724 multi-link).

## Device tree

Two properties. The serializer states its link; the deserializer states the map.

```dts
	dser: max96712@29 {
		compatible = "maxim,max96712";
		link-mask = <0x3>;
		/* <serializer-vc deserializer-vc> */
		maxim,link0-vc-remap = <0 0>, <1 1>, <2 2>, <3 3>;
		maxim,link1-vc-remap = <0 4>, <1 5>, <2 6>, <3 7>;
	};

	ser_a: max96717@40 {
		maxim,gmsl-link-id = <0>;
		maxim,gmsl-dser-device = <&dser>;
	};

	ser_b: max9295_b@41 {
		maxim,gmsl-link-id = <1>;
		maxim,gmsl-dser-device = <&dser>;
	};
```

The tuple grouping is cosmetic — `<0 0>, <1 1>` and `<0 0 1 1>` compile to the
same cell array. The driver enforces the stride of 2.

### Rules

- **No table for a link** → unity map, all 8 serializer VCs map to themselves.
  Usable as-is only on a single-link deserializer.
- **A table for a link** → it replaces the unity default outright. VCs the table
  does not list are left *unmapped* and rejected at stream start. Declare exactly
  the streams the camera uses (today: 4).
- **A multi-link deserializer must declare a table for every enabled link.** Two
  links both emitting VC 0-3 would be indistinguishable on the shared CSI output,
  so the unity default cannot work there. Probe fails with
  `dser VC <n> mapped more than once`.
- Deserializer VCs are 5-bit (0-31); serializer VCs are 0-7.
- `maxim,gmsl-link-id` is optional. Without it d4xx falls back to
  `vc-id / 4` and logs `no maxim,gmsl-link-id, using link N from vc-id M`,
  which keeps pre-existing overlays working.

## The deserializer VC appears four times per stream

A remap is only correct if all four agree. Changing the table alone does nothing
useful — the Tegra side still demultiplexes on the old VC.

| where | consumer |
|---|---|
| `maxim,linkN-vc-remap` on the deserializer | max96712 register programming |
| `ds5_N/ports/port@0/endpoint` `vc-id` | sensor's declared output VC |
| `ds5_N/gmsl-link/vc-id` | d4xx `g_ctx.dst_vc` |
| `tegra-capture-vi/ports/port@N/endpoint` `vc-id` | VI channel demux |

The serializer VC never appears in DT. It is the stream index the camera itself
uses — depth 0, RGB 1, IR 2, IMU 3 — and the driver recovers it by inverting the
table.

## Driver

`dser_interface` ops take the link explicitly. `vc_id` in these calls is the
**serializer** VC.

| op | argument |
|---|---|
| `get_multi_vc_pipe_id` | `link` |
| `bind_ser_to_dser_pipe` | `link` |
| `set_pipe` | `link`, `vc_id` |
| `reset_oneshot_link` | `link` |
| `get_ser_vc_id` | `link`, `dser_vc` → serializer VC |

`get_ser_vc_id` is the reverse lookup, and is `NULL` on deserializers that do not
remap (where the two VCs are the same). d4xx uses it in `ds5_configure()` to
resolve the serializer VC once per configure.

In the max96712 registers the two VCs are separate fields: the `SRC_n_MAP`
register carries the incoming serializer VC, `DST_n_MAP` the outgoing one, and
`MIPI_TX_EXT<n>` carries the high bits of both — destination at bits 4:2, source
at bits 7:5. They coincide only under the old `link * 4 + ser_vc` layout.

**Camera-side registers use the serializer VC.** `DS5_*_STREAM_MD` is written to
the camera, so its VC field is the serializer VC, not `dst_vc`. Getting this
wrong is invisible under an identity map and breaks only the streams that carry
embedded metadata (depth/RGB/IR, not IMU), because the metadata lands on a VC the
deserializer has no matching source entry for and the VI channel waits for a line
that never arrives.

## Limits

- The multi-VC pipe (MAX96717) programs all four of a camera's VCs from one
  register table, so all four must be mapped. `set_pipe` returns `-EINVAL`
  otherwise.
- A duplicate *source* VC within one table is not diagnosed; the last pair wins.
- Every generation shares one `d4xx.c`, so the max96712 change ships via
  `nvidia-oot/6.0/0003` (JP6.1/6.2 and JP7 symlink it) and must be ported to the
  JP5 carriers `kernel/nvidia/5.1.2/0007` and `5.0.2/0017`.

## Example: swapping depth across two links

Both cameras emit depth on their own serializer VC 0. To have link 0's depth
leave on VC 4 and link 1's on VC 0:

```dts
	maxim,link0-vc-remap = <0 4>, <1 1>, <2 2>, <3 3>;
	maxim,link1-vc-remap = <0 0>, <1 5>, <2 6>, <3 7>;
```

then set `vc-id` to 4 on link 0's depth `ds5` node (endpoint and `gmsl-link`) and
on its VI channel, and to 0 on link 1's. The union of both tables stays 8 distinct
VCs, so the collision check passes; swapping only one side would fail probe.

Confirm from dmesg — `max96712_set_pipe` logs the resolved pair:

```
max96712_set_pipe pipe_id 0, ... link 0, vc_id 0 -> dser vc 4
max96712_set_pipe pipe_id 1, ... link 1, vc_id 0 -> dser vc 0
```
