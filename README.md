# AI Driver Behaviour

**Beta, no promises.** For F1 25 on Steam with Swofty Launcher 1.7.0 or newer.

How the AI drivers race other cars, how they handle slides and damage, and how hard they push. Works together with **Contact Physics**, which covers what a hit does.

## Settings, in the order of the settings page

**Close racing**
- **Gap to other cars:** how much room an AI car leaves around other cars. A smaller gap means closer, harder racing. Same for every car.
- **When they make room:** when a car is next to an AI car, how far alongside it must be before the AI moves over. Later means the AI holds its line and races wheel to wheel.
- **Drivers differ:** spreads "When they make room" between drivers: each driver gets their own value around the one above, and attacks from earlier or later and stays in a pass longer or shorter. A driver has the same style in every race. Small, Medium and Big set how far apart the drivers are; Off means all drivers use the plain value. The gap to other cars is not spread, because the game stores it per team car, not per driver. It works on the game's own drivers; a driver added by a mod uses the plain values.

**Slides** (how the AI handles its car when it slides)
- **How well they catch them**, **Wheelspin out of corners**, **When they lift off**, **When they lose the car:** make the AI catch slides later, spin out more easily, or keep pushing in a slide.

**Damage**
- **Speed with a damaged car:** how much a damaged AI car slows down.

**Pace** was removed in 0.1.7: the AI planned corner speeds from more grip than the car has and hit the barriers on tight tracks such as Monaco. It may come back as a track-aware option.

The Close racing, Slides and Damage settings apply after you restart the game.

## Planned
Making each driver's style follow their ratings (racecraft, awareness, experience, focus), so a driver added by a mod races the way their ratings say. Not built yet.

## Good to know
- Replaces the racecraft, car control and driver variation settings of the old **AI Racing Tweaks** (versions before 0.5.0, now called Contact Physics). Update that one first.
- The "every driver races differently" style is picked from the driver's name in the game database, not from their ratings. A driver keeps the same style in every race.

MIT licence. Made by Kizi. Thanks to Swofty for the launcher and the SDK.
