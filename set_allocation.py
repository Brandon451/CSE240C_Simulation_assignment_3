list_rand = []

#for n in range(0, 64):
#    list_rand.append(33*n)

#for n in range(1, 66):
#    list_rand.append(31*n)

for n in range(0, 2048):
    if (((n>>6) & 0x1F) == (n & 0x1F)):
        list_rand.append(n)
    if (((~n>>6) & 0x1F) == (n & 0x1F)):
        list_rand.append(n)

list_rand.sort()
set_rand = set(list_rand)

print(len(list_rand) != len(set_rand))

print(list_rand)
print(len(list_rand))
