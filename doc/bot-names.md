# Bot names: first name + surname (`data/bot_names.txt`)

The game client shows a character as "Name Surname" (`characters.surname`, up to 48 characters; the pair is unique on the realm). Bots used to get only a generated first name. The core (`BotMgr::CreateBot`) now reads this file (`Bot.Names.File`, empty = the old behaviour: a generated first name, no surname) and gives each new bot a first name and a surname.

## Row format

One row per line, `|` separated, `#` comments, blank lines ignored.

    kind|style|races|classes|sex|weight|text
    pair|style|races|classes|sex|weight|first|last

| Field | Values |
|---|---|
| kind | `first` (one word, letters only, 2-12 characters, the player-name rule), `last` (surname: letters, space, apostrophe, hyphen; 2-24 characters), `pair` (a fixed full name) |
| style | `normal` everyday fantasy names, `funny` jokes and class puns (Stabsworth for rogues, Shotgunson for hunters), `lore` names from the game's own world |
| races | `*` or comma list of race ids (1 human, 2 orc, 3 dwarf, 4 night elf, 5 undead, 6 tauren, 7 gnome, 8 troll, 10 blood elf, 11 draenei) |
| classes | `*` or comma list of class ids (1 warrior, 2 paladin, 3 hunter, 4 rogue, 5 priest, 7 shaman, 8 mage, 9 warlock, 11 druid) |
| sex | `*`, `m` or `f` |
| weight | 1..100, how often the row is drawn among the rows of the same kind and style that fit the bot |

## How a bot is named

1. The core picks a style by `Bot.Names.StyleWeights` (default `normal:60,funny:25,lore:15`).
2. A `pair` row of that style that fits the bot's race, class and sex is used with probability `Bot.Names.PairChance` (default 0.05).
3. Otherwise the first name is a `first` row of that style that fits (race, class, sex), or the generated syllable name when no row fits (always the case for most bots: the generator stays); the surname is a `last` row of that style that fits (race, class). If no row of the style fits, the `normal` style with `*` races is used.
4. The pair "first + surname" must be unique on the realm and the first name must pass the player-name check; otherwise it is redrawn (up to 200 times, then the surname is dropped).
5. Rows that break a rule are skipped and logged at start (`BotMgr: names file line N: ...`).

## Adding names

Append rows to the right section of the file. Keep one name per row, no duplicate (kind, name, races, sex) rows. Funny surnames go in the class section so that a rogue gets a rogue joke. Check with `python3 tools/check_bot_names.py` (rule check, duplicate check) before committing.

## Guild names (`data/bot_guilds.txt`)

One row per name: `guild|style|weight|name`. Styles: `lore`, `funny`, `normal`, `reallife`. Weight 1 to 100. There is no faction or race column: any bot guild may take any name.
Name rule (guild name): letters, space, apostrophe, hyphen, 2 to 24 characters, unique ignoring case. Check with `python3 tools/check_bot_guilds.py`.
The core does not read this file yet.
