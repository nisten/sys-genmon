# sys-genmon

<div align='center'>
  
## A very pretty system monitor for xfce4-genmon

<img src='genmon_bars.png'>

| Color              | Meaning                |
| ------------------ | ---------------------- |
| Blue (alternating) | CPU core utilization   |
| Yellow             | CPU memory utilization |
| Purple             | Swap utilization       |
| Green              | GPU SM Utilization     |
| Yellow-Green       | GPU memory utilization |

Only Nvidia GPUs are supported at the moment, as I don't have one to test with. AMD would be red :). Contributions welcome.

</div>

## Installation

1. Compile `sys-genmon.c`. You can use `./build.sh`, or do it yourself, it's just one file.
2. Create an xfce4-genmon plugin on your panel. My panel is horizontal, with size 26.
3. Set the plugin to execute `/path/to/binary -t -s -p`. You can use `--help` to see what the options do, or read the code for yourself.