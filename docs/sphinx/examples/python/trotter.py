# ============================================================================ #
# Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

import cudaq
import time
import matplotlib.pyplot as plt
import numpy as np

cudaq.set_target('nvidia-fp64')

# Alternating up/down spins
@cudaq.kernel
def getInitState(numSpins:int):
    q = cudaq.qvector(numSpins)
    for qId in range(0, numSpins, 2):
        x(q[qId])
    

# This performs a single-step Trotter on top of an initial state, e.g.,
# result state of the previous Trotter step.
@cudaq.kernel
def trotter(initialState:cudaq.State, coefficients:list[complex], words:list[cudaq.pauli_word], dt:float):
    q = cudaq.qvector(initialState)
    for i  in range(len(coefficients)):
        exp_pauli(coefficients[i].real * dt, q, words[i])
    
def run_steps(steps: int, spins: int):
    g = 1.0
    Jx = 1.0
    Jy = 1.0
    Jz = g
    dt = 0.05
    n_steps = steps #100
    n_spins = spins # 25
    omega = 2 * np.pi

    def heisenbergModelHam(t:float) -> cudaq.SpinOperator:
        tdOp = cudaq.SpinOperator(num_qubits=n_spins)
        for i in range(0, n_spins-1):
            tdOp += (Jx * cudaq.spin.x(i) * cudaq.spin.x(i + 1))
            tdOp += (Jy * cudaq.spin.x(i) * cudaq.spin.y(i + 1))
            tdOp += (Jz * cudaq.spin.x(i) * cudaq.spin.z(i + 1))
        for i in range(0, n_spins):
            tdOp += (np.cos(omega * t) * cudaq.spin.x(i))
        return tdOp

    # Collect coefficients from a spin operator so we can pass them to a kernel
    def termCoefficients(op:cudaq.SpinOperator) ->list[complex]:
        result = []
        ham.for_each_term(lambda term: result.append(term.get_coefficient()))
        return result
    
    # Collect pauli words from a spin operator so we can pass them to a kernel
    def termWords(op:cudaq.SpinOperator) ->list[str]:
        result = []
        ham.for_each_term(lambda term: result.append(term.to_string(False)))
        return result

    # Observe the average magnetization of all spins (<Z>)
    average_magnetization = 0.0
    for i in range(0, n_spins):
        average_magnetization += ((1.0 / n_spins) * cudaq.spin.z(i))
    average_magnetization -= 1.0

    # Run loop
    state = cudaq.get_state(getInitState, n_spins)

    exp_results = []
    times = []
    for i in range(0, n_steps):
        start_time = time.time()
        ham = heisenbergModelHam(i * dt)
        coefficients = termCoefficients(ham)
        words = termWords(ham)
        magnetization_exp_val = cudaq.observe(trotter, average_magnetization, state, coefficients, words, dt)
        exp_results.append(magnetization_exp_val.expectation())
        state = cudaq.get_state(trotter, state, coefficients, words, dt)
        times.append(time.time() - start_time)
    
    plot = plt.plot(list(range(len(times))), times)
    plt.xlabel("steps")
    plt.ylabel("time")
    plt.savefig("img.png")

start_time = time.time()
run_steps(100, 25)
print(f"total time: {time.time() - start_time}s")
