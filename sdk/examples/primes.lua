-- primes.lua: sieve of Eratosthenes, timing with os.clock().
local n = tonumber(arg and arg[1]) or 100000
local t0 = os.clock()
local sieve, count = {}, 0
for i = 2, n do
  if not sieve[i] then
    count = count + 1
    for j = i * i, n, i do sieve[j] = true end
  end
end
print(string.format("%d primes below %d (%.2f s)", count, n, os.clock() - t0))
