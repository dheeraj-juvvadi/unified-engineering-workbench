# Task F: Backpropagation worked derivation (typed)

> The handwritten scan requested by the brief is **not included** in this submission; this typed
> derivation shows the same steps (forward pass, chain rule, one update).
> Everything below was re-computed with Python (8 decimals).
> The Workbench "Backprop" tab (`src/core/backprop.*`) should print the same numbers; use it as your final check.

**Problem.** One neuron followed by a linear output, one training example.

```
x = 1,  y = 2                      (input, target)
z = w x + b,   h = tanh(z)         (hidden)
yhat = v h + c                     (output)
J = 1/2 (yhat - y)^2               (loss)
start: w = 0.5, b = 0, v = 1, c = 0      learning rate lr = 0.1
```

---

## PAGE 1: Forward pass and gradients

### 1. Forward pass (numbers)

| step | formula | value |
|---|---|---|
| z | w·x + b = 0.5·1 + 0 | **0.5** |
| h | tanh(0.5) | **0.46211716** |
| ŷ | v·h + c = 1·0.46211716 + 0 | **0.46211716** |
| e | ŷ − y = 0.46211716 − 2 | **−1.53788284** |
| J | ½·e² = ½·(1.53788284)² | **1.18254182** |

(e is the "error". It is negative because the prediction 0.46 is below the target 2.)

### 2. Chain rule, symbolic first

Work backwards from J. Each line reuses the line above it.

```
dJ/dyhat = e = yhat - y
dJ/dv    = dJ/dyhat * dyhat/dv = e * h          (since yhat = v h + c, dyhat/dv = h)
dJ/dc    = dJ/dyhat * dyhat/dc = e * 1 = e
dJ/dh    = dJ/dyhat * dyhat/dh = e * v          (dyhat/dh = v)
dh/dz    = 1 - h^2                              (derivative of tanh)
dJ/dz    = dJ/dh * dh/dz = e * v * (1 - h^2)
dJ/dw    = dJ/dz * dz/dw = e * v * (1 - h^2) * x      (dz/dw = x)
dJ/db    = dJ/dz * dz/db = e * v * (1 - h^2)          (dz/db = 1)
```

### 3. Same thing with numbers (all at the OLD parameters)

First the helper: 1 − h² = 1 − 0.46211716² = **0.78644773**.

| gradient | formula | value |
|---|---|---|
| ∂J/∂ŷ | e | −1.53788284 |
| ∂J/∂c | e | **−1.53788284** |
| ∂J/∂v | e·h = −1.53788284 · 0.46211716 | **−0.71068205** |
| ∂J/∂h | e·v = −1.53788284 · 1 | −1.53788284 |
| ∂J/∂z | e·v·(1−h²) = −1.53788284 · 0.78644773 | −1.20946448 |
| ∂J/∂w | ∂J/∂z · x = −1.20946448 · 1 | **−1.20946448** |
| ∂J/∂b | ∂J/∂z | **−1.20946448** |

dJ/dw equals dJ/db only because x = 1 (dz/dw = x = 1). With another x they would differ.

*Suggested page break here. Page 2 starts with the update.*

---

## PAGE 2: Update, new loss, and what it means

### 4. One simultaneous gradient-descent step (lr = 0.1)

Rule: new = old − lr · gradient. All four gradients above were computed at the **old** (w, b, v, c) first; only now do we change anything.

```
w_new = 0.5 - 0.1*(-1.20946448) = 0.5 + 0.12094645 = 0.62094645
b_new = 0   - 0.1*(-1.20946448) = 0 + 0.12094645   = 0.12094645
v_new = 1   - 0.1*(-0.71068205) = 1 + 0.07106820   = 1.07106820
c_new = 0   - 0.1*(-1.53788284) = 0 + 0.15378828   = 0.15378828
```

### 5. New forward pass and loss

```
z1 = w_new*1 + b_new = 0.62094645 + 0.12094645 = 0.74189290
h1 = tanh(0.74189290)                          = 0.63028744
yhat1 = v_new*h1 + c_new = 1.07106820*0.63028744 + 0.15378828 = 0.82886912
e1 = 0.82886912 - 2 = -1.17113088
J1 = 0.5 * (1.17113088)^2 = 0.68577377
```

| | before | after |
|---|---|---|
| ŷ | 0.46211716 | 0.82886912 |
| J | 1.18254182 | **0.68577377** |

J dropped by 0.49676805 in one step. (First-order estimate: lr·‖∇J‖² = 0.1 · 5.79576 = 0.5796. It is a bit larger than the real drop of 0.4968 because the loss curves, and the step is not tiny.)

### 6. Why the gradients point the way they do

- A gradient tells you how J changes if you **increase** that parameter. Here **all four are negative**: increasing any of w, b, v or c would **lower** J.
- Gradient descent moves each parameter by −lr·gradient, which is a **positive** amount, so **all four parameters increase**.
- Why that is sensible: ŷ = 0.46 is far below the target 2, so e < 0 and we need ŷ to **rise**. Bigger c adds directly to ŷ. Bigger v scales h (h > 0, so ŷ rises). Bigger w or b pushes z up, so h = tanh(z) rises, and with v > 0 so does ŷ.
- Result: ŷ goes 0.462 → 0.829, toward 2, and J falls 1.183 → 0.686.
- Relative sizes: c and w/b have large gradients (−1.54, −1.21). v has a smaller one (−0.71) because it is scaled by h = 0.46. The tanh slope 0.786 shrinks the path through w and b a little.

### 7. Why "simultaneous" matters

The gradient is a statement about the loss surface **at one point** (w, b, v, c) = (0.5, 0, 1, 0). The step is only justified for that point. If you updated w first and then computed ∂J/∂b or ∂J/∂v using the new w (or any other mixed set), you would be using gradients from a point that does not exist in the plain gradient-descent rule, and you would no longer be following −∇J. Notice that dJ/dw, dJ/db and dJ/dh also use the old v, and dJ/dv uses the old h. So: compute everything, then update everything.

---

### Quick self-check list

- [ ] z = 0.5, h = 0.46211716, ŷ = 0.46211716, e = −1.53788284, J = 1.18254182
- [ ] 1−h² = 0.78644773; dJ/dw = dJ/db = −1.20946448; dJ/dv = −0.71068205; dJ/dc = −1.53788284
- [ ] w = 0.62094645, b = 0.12094645, v = 1.07106820, c = 0.15378828
- [ ] J1 = 0.68577377 (smaller than J0, as it should be)
