
import random
from collections import defaultdict
from itertools import product
import copy

def guess_result(real: str, guess: str):

    if len(guess) != 4:
        raise("guess should be 4 digits")

    answer = ""
    guess_so_far = ""
    for i,d in enumerate(guess):
        if d not in real:
            answer+="R"
        elif d == real[i]:
            answer+="G"
        elif d in real and d != real[i]:

            if guess_so_far.count(d) == real.count(d):
                answer+="R"
            else:
                answer+="Y"

        guess_so_far+=d

    return answer

def generate_random():
    x = ""
    for _ in range(4): x+=str(random.randint(0,9))
    return x

def is_complete(correct: list):

    for g in correct: 
        if g == "-": return False
    return True

def filter_guesses(guesses, not_in_pos, correct, guessed):
    return list(dict.fromkeys(
        guess for guess in guesses
        if guess not in guessed
        and all(j not in not_in_pos[d] for j, d in enumerate(guess))
        and all(str(d) in guess for d in not_in_pos)
        and all(correct[j] == "-" or correct[j] == guess[j] for j in range(4))
    ))[::-1]

f = 0
debug = False
# candidates = [f"{i:04d}" for i in range(10000)]
# candidates = ["4514"]
# for x in candidates:
for _ in range(pow(10,7)):
    x = generate_random()
    found = False
    c = 1

    if _%100000 == 0:
        print("Progress: ", _/pow(10,7)*100, "%")

    guesses = ["8900", "4567", "0123"]
    guessed = []
    existing_nums = set()
    correct = ["-", "-", "-", "-"]
    not_in_pos = defaultdict(list)
    in_pos = defaultdict(list)

    while not found:
        
        if debug:
            print("Guess number: ", c)
        if c > 7:
            print("Failed to guess in 7 tries, the number was: ", x)
            f+=1
            break

        
        if c > 3:
        
            for k in in_pos:
                if len(in_pos[k]) == 1:
                    correct[in_pos[k][0]] = k

            if is_complete(correct):
                guesses.append("".join(correct))
                guesses = filter_guesses(guesses, not_in_pos, correct, guessed)
            else:
                empty = []
                for i in range(4):
                    if correct[i] == "-":
                        empty.append(i)
                
                possible_values = [
                    [d for d in in_pos if e in in_pos[d]]
                    for e in empty
                ]

                for combination in product(*possible_values):
                    for i, value in zip(empty, combination):
                        correct[i] = value

                    if is_complete(correct):
                        guesses.append("".join(correct))

                    for i in empty: correct[i] = "-"    

                guesses = filter_guesses(guesses, not_in_pos, correct, guessed)

                if guesses == []:
                    print("No more guesses left, the number was: ", x)
                    f+=1
                    break

        guess = guesses.pop()
        guessed.append(guess)
        if guess == x:
            # print("Correct guess yeeeeeeeeeeeeeeeeeeeey!!!")
            found = True

        res = guess_result(x, guess=guess)

        if debug:
            print(guess)
            print(res)

        for i, r in enumerate(res):
            if r == "G":
                correct[i] = guess[i]
                existing_nums.add(guess[i])
                in_pos[guess[i]].append(i)
            elif r == "Y":
                existing_nums.add(guess[i])
                not_in_pos[guess[i]].append(i)

        for e in existing_nums:

            empty = []
            for i in range(4):
                if correct[i] == "-":
                    empty.append(i)

            for p in empty:

                if p not in not_in_pos[e]:
                    if correct[p] == "-": in_pos[e].append(p)
                if p in not_in_pos[e] and p in in_pos[e]:
                    in_pos[e] = list(filter(lambda x: x!= p, in_pos[e]))

            not_in_pos[e] = list(set(not_in_pos[e]))
            in_pos[e] = list(set(in_pos[e]))

        if debug:
            print("Current guess: ", guess)
            print("Correct so far: ", correct)
            print("Existing numbers: ", existing_nums)
            print("Not in pos: ", not_in_pos)
            print("In pos: ", in_pos)
            print("Guesses left: ", guesses[::-1])

        c+=1

# print("Failed to guess in 7 tries: ", f, " out of ", len(candidates), " numbers")
print("Failed to guess in 7 tries: ", f, " out of ", pow(10, 8), " numbers")
