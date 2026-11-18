set debuginfod enabled on
set pagination off
set breakpoint pending on
break SDL_CreateRenderer
run
delete
break *(VULKAN_SetDrawState+759)
commands
silent
set $n = $n + 1
if $rcx > $max
set $max = $rcx
end
if $rcx + 48 > 65536
printf "OVERRUN write at offset %lu (+48 > 65536), write #%d\n", $rcx, $n
end
continue
end
set $n = 0
set $max = 0
continue
printf "writes=%d max_offset=%lu\n", $n, $max
