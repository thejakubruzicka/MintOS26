-- files.lua: write, read back and list files on MintOS HD with Lua's io library.
local path = (os.getenv("HOME") or "/Users/mintos") .. "/Documents/lua-test.txt"
local f = assert(io.open(path, "w"))
for i = 1, 5 do f:write(string.format("line %d: %s\n", i, string.rep("*", i))) end
f:close()
local lines = 0
for line in io.lines(path) do lines = lines + 1; print(line) end
print(("read %d lines back from %s"):format(lines, path))
