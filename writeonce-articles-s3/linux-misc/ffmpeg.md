```sh
ffmpeg -video_size 1920x1080 -framerate 30 -f x11 grab -i :1 -c:v h264_nvenc output.mp4


ffmpeg -i trimmed.mp4 -i palette.png -filter_complex "fps=10,scale=1280:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5" grepjson.gif
ffmpeg -i trimmed.mp4 -vf "fps=10,scale=1280:-1:flags=lanczos,palettegen" palette.png
ffmpeg -i trimmed.mp4 -i palette.png -filter_complex "fps=10,scale=960:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5" grepjson.gif
ffmpeg -i trimmed.mp4 -vf "fps=10,scale=960:-1:flags=lanczos,palettegen" palette.png
ffmpeg -ss 5 -t 30 -i output.mp4  -c copy trimmed.mp4
```
