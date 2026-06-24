# Player Character & Modular Equipment Overhaul

Branch: `player-equipment-overhaul`. Goal: replace the chibi-blob player model with a
proportioned low-poly humanoid wearing **modular, tiered, recolourable equipment** that matches
the reference art (a Knight, Mage, Hunter, Cleric). Players start in **ragged clothes** and buy
**better gear from town shops** that both **looks nicer and makes them more powerful**, can
**recolour** their outfit, and **change weapons**. The animation rig must **blend** locomotion with
upper-body actions (walk + swing/block/cast at once).

## Reference art
The 4 character references were attached in chat (NOT on disk — `docs/Reference Images/` holds only
the world/town art). Detailed specs are recorded here so every loop can build toward them without the
image. Common to all: a **realistic adult low-poly humanoid** (~7-8 heads tall, faceted flat shading),
standing idle, neutral desert backdrop. This is the look — the current rig is a chibi blob and must be
reproportioned (item 3).

- **Knight** — full **polished steel plate** (light blue-grey) with **gold trim/filigree**: breastplate,
  pauldrons, gorget, fauld, vambraces, gauntlets, greaves, sabatons. **Great-helm** with eye-slit
  (faint blue glow) topped with a **red plume**. Right hand rests a **steel longsword** point-down on
  the ground (gold crossguard); left arm a **heater shield** (royal-blue field, gold rampant **lion**,
  gold border). Palette: steel, gold, royal blue, red.
- **Mage** — long **deep-purple robe** with **gold trim** down the front and on the collar, a layered
  shoulder cape, **purple hood up** over a shadowed bearded face with **glowing cyan eyes**. Brown
  leather belt + pouches, gloves, brown boots. Left hand hugs a **brown spellbook** at the hip; right
  hand a tall **wooden staff** topped with a claw cradling a **floating glowing blue orb** (icosahedron)
  with small shards orbiting. Palette: indigo/purple, gold, brown, glowing cyan.
- **Hunter** — muted **olive-green + brown leather** ranger: cloth **cap/hood** + a **face mask** over
  nose/mouth (only stern eyes show), leather **jerkin** with clasps, a diagonal **bandolier** strap with
  pouches, a waist belt with pouches, forearm **bracers**, gloves, trousers into brown **boots**. A
  **quiver of arrows on the back** (fletching jutting above both shoulders). Left hand a recurve **bow**,
  right hand a **dagger**. Palette: olive, tan, brown leather, dark steel.
- **Cleric** — **white robe/tabard** with **royal-blue + gold** trim: gold-edged pauldrons, a gold
  gorget/collar, a blue front tabard panel. Head is a steel helm (visor slit) under a tall **golden
  bishop's mitre**, blue hood/coif over the shoulders. Right hand a golden flanged **mace**; left a
  blue+gold **kite shield** with a gold **cross** (cross-in-circle) emblem. White robe-skirt to the
  ankles, brown boots. Palette: white, royal blue, gold, steel.

## Design
- **Equipment = outfit + weapon** (modular but "one piece for the look"): an **outfit** (the whole
  worn set, themed by role) with a **tier** (Ragged → Worn → Fine → Master) and a player-chosen
  **colour**, plus a **weapon** (type + tier + colour) attached to the hand. Higher tier = nicer mesh
  **and** a stat bonus on top of `role_stats`.
- **Outfit = extra rig bones** appended to the humanoid skeleton (like the existing hair/face feature
  bones), so the animator poses + the gear animates for free (a robe skirt on the pelvis sways, a
  pauldron on the shoulder swings). Recolour via an extended `CharacterPalette`.
- **Server-authoritative**: `Equipment` rides in `PlayerInput`/`PlayerState`; the server stores it,
  applies the tier stat bonus, and gates purchases against the party wallet (`money_`).

## Action list (one item per loop)
1. **[done] Render harness + plan.** `asset_preview character <role>` bakes a posed `CharacterModel`
   (+ weapon) to a PNG, lit/framed like the references. Baseline-render the current model.
2. **Equipment data model** (`Character/Equipment.h`): tiers, colours, `equipment_stats`. Tested.
3. **Proportioned humanoid rig**: replace the chibi `generate()` with a taller, real-proportioned
   low-poly human; keep bone parts/indices stable for the animator. Update `character_tests`.
4. **Outfit builder** (`Character/Outfit.{h,cpp}`): `apply_outfit(model, role, equipment)` appends
   themed outfit bones per role + tier, coloured by the equipment palette. Wire into `create()`.
5. **Knight** outfit tiers (plate, plume, trim) — iterate vs reference.
6. **Mage** outfit tiers (hooded robe, gold trim, glow) — iterate vs reference.
7. **Hunter** outfit tiers (leather, hood/mask, quiver, straps) — iterate vs reference.
8. **Cleric** outfit tiers (mitre, white/blue/gold robe, cross) — iterate vs reference.
9. **Modular weapons** (`Character/Weapon.{h,cpp}`): sword/bow/staff/mace/shield by type+tier+colour,
   attached to the hand joint; replace the hardcoded `draw_role_weapon`; allow swapping.
10. **Animation polish**: walk + swing/block/cast blend on the new rig; add a cast overlay; verify
    headless (blade-tip trace) + a posed mid-swing render.
11. **Networking + stats**: `Equipment` in the protocol; server stores it per player + folds
    `equipment_stats` into `sync_player_role`. Protocol round-trip test.
12. **Shop interaction**: walk up to a town shop (blacksmith); a shop panel buys outfit-tier upgrades
    + weapons with the party `money_` (server-authoritative).
13. **Wardrobe/upgrade UI**: an in-game panel (key) to equip owned gear, recolour, change weapon.
14. **Customise-screen integration**: starting outfit colour pickers + the new model in the preview.
15. **Final pass**: render every role × tier vs the references, tune, full test sweep, update CLAUDE.md.

## Conventions
- Keep it **headless-testable** (pure data + maths in `Character/`); the client only renders.
- Re-render the affected role(s) with `make character` after each change and compare to the reference.
- Every loop ends green (build + tests) and is committed.
