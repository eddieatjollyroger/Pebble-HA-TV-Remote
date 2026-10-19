# Pebble HA TV Remote turns your watch into a TV remote using Home Assistant.<br/>


### It works with Apple TV, Android TV (through the Android TV Remote integration) and VIDAA TVs (through the Vidaa TV integration).


<img width="200" height="228" alt="pebbleapp10" src="https://github.com/user-attachments/assets/dc4f8ec7-dbcc-4672-8c16-1eea866d78b5" />
<img width="200" height="228" alt="pebbleapp11" src="https://github.com/user-attachments/assets/21784857-a74b-452c-b993-221843cab11f" />
<img width="200" height="228" alt="pebbleapp12" src="https://github.com/user-attachments/assets/a1a57058-b28d-4aec-8b11-5c6231bbacdd" /><br/>


<img width="200" height="228" alt="pebbleapp20" src="https://github.com/user-attachments/assets/fa1cca4a-a184-43cd-9e9e-35e57bca3c9f" />
<img width="200" height="228" alt="pebbleapp21" src="https://github.com/user-attachments/assets/57fd6999-2034-445c-ade0-e1af91731530" />
<img width="200" height="228" alt="pebbleapp22" src="https://github.com/user-attachments/assets/74007120-7198-4b51-a52c-88dbbcd7733e" />

## Setup

Open the app’s settings on your phone.<br/>
Enter your Home Assistant URL and a long-lived access token. You can create one in Home Assistant under your profile, Security tab.<br/>
Pick dark or light theme, then save. The theme applies to the watch too.

## The screen

The top line shows the active TV and its state, such as [PLAY], [PAUSE] or [OFF].<br/>
Below it is what’s playing, with the artist or series and the app. On Apple TV you also get a progress bar and times. Android and VIDAA TVs only report the app name.<br/>
The bottom line shows the current button mode and the time.

## Buttons

| Button | What it does |
| --- | --- |
| Select | OK |
| Hold Select | Switches mode: left/right, then up/down, then volume.<br/> The current mode is shown at the bottom of the screen |
| Hold Up or Down | Opens the watch app menu |
| Back | Sends a Back command to the TV |
| Double-tap Back or Long press Back | Exits the app |


## Modes

left/right (default): Up moves left, Down moves right.<br/>
up/down: Up and Down move up and down.<br/>
volume: Up and Down change the volume.

## Menu
Hold Up or Down to open it. Up and Down scroll and wrap around, and Select chooses.

select tv: opens the list of TVs. Pick one to control it.
power on / power off
home and home (hold)

## Notes

- A tag like [NO MP] means the app couldn’t find a media player for that TV, so now-playing info isn’t available. Controls still work.
- Long TV names slide sideways so you can read them.
- When idle, the screen only refreshes once a minute to save battery.
