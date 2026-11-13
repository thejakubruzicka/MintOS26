-- hello.lua: a first Lua script on MintOS.
print("Hello from Lua " .. _VERSION .. " on MintOS!")
print(string.format("pi = %.10f, sqrt(2) = %.10f, 2^0.5 = %g", math.pi, math.sqrt(2), 2 ^ 0.5))
local words = {}
for w in ("the quick brown fox jumps over the lazy dog"):gmatch("%a+") do words[#words + 1] = w end
table.sort(words)
print("sorted: " .. table.concat(words, " "))
print("upper: " .. ("mintos"):upper() .. ", reversed: " .. ("mintos"):reverse())
print("os.time() = " .. os.time() .. ", os.date() = " .. os.date("%Y-%m-%d %H:%M:%S"))
