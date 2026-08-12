// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

import std.collection.ArrayList

foreign func MRT_RunEpochHandshake(): UInt64
foreign func MRT_StartEpochHandshakeForeignChurn(): Unit
foreign func MRT_StopEpochHandshakeForeignChurn(): UInt64

class EpochGarbageNode {
    let value: Int64
    let next: Option<EpochGarbageNode>

    init(value: Int64, next: Option<EpochGarbageNode>) {
        this.value = value
        this.next = next
    }
}

func AllocateEpochGarbage(seed: Int64, count: Int64): Int64 {
    var head: Option<EpochGarbageNode> = None
    for (index in 0..count) {
        head = Some(EpochGarbageNode(seed + index, head))
    }
    match (head) {
        case Some(node) => node.value
        case None => -1
    }
}

func RunCreateExitWave(rounds: Int64): Int64 {
    var checksum: Int64 = 0
    for (round in 0..rounds) {
        let wave = ArrayList<Future<Int64>>()
        for (index in 0..64) {
            let captured = round * 64 + index
            wave.add(spawn {
                =>
                    if (captured % 3 == 0) {
                        sleep(Duration.millisecond)
                    }
                    captured
            })
        }
        for (future in wave) {
            checksum += future.get()
        }
    }
    checksum
}

func RunAllocationWave(rounds: Int64): Int64 {
    var checksum: Int64 = 0
    for (round in 0..rounds) {
        checksum += AllocateEpochGarbage(round, 40000)
        sleep(Duration.Zero)
    }
    checksum
}

main(): Int64 {
    unsafe { MRT_StartEpochHandshakeForeignChurn() }
    let parked = ArrayList<Future<Int64>>()
    for (index in 0..64) {
        let captured = index
        parked.add(spawn {
            =>
                sleep(750 * Duration.millisecond)
                captured
        })
    }

    // dynjoin: keep create/exit churn fully concurrent with every handshake so
    // mid-handshake creates (deferred_create / born_clean) are forced live.
    let creator = spawn { => RunCreateExitWave(400) }
    let allocator = spawn { => RunAllocationWave(64) }
    sleep(2 * Duration.millisecond)

    var lastEpoch = UInt64(0)
    var explicitEpochs: Int64 = 0
    for (round in 0..1500) {
        let epoch = unsafe { MRT_RunEpochHandshake() }
        if (epoch <= lastEpoch) {
            println("EPOCHHS_STRESS_FAIL round=${round} epoch=${epoch} last=${lastEpoch}")
            return 2
        }
        lastEpoch = epoch
        explicitEpochs++
        // Spawn a short-lived wave on every 8th handshake so create races the wait.
        if (round % 8 == 0) {
            let burst = spawn {
                =>
                    var sum: Int64 = 0
                    for (i in 0..16) {
                        sum += (spawn { => i }).get()
                    }
                    sum
            }
            let _burstSum = burst.get()
        }
        if (round % 4 == 0) {
            sleep(Duration.millisecond)
        } else {
            sleep(Duration.Zero)
        }
    }
    let foreignStats = unsafe { MRT_StopEpochHandshakeForeignChurn() }
    let foreignScale = UInt64(4294967296)
    let foreignAttempts = foreignStats % foreignScale
    let foreignAttached = foreignStats / foreignScale

    let createExitChecksum = creator.get()
    let allocationChecksum = allocator.get()
    var parkedChecksum: Int64 = 0
    for (future in parked) {
        parkedChecksum += future.get()
    }

    let expectedCreateExit: Int64 = 20476800
    let expectedParked: Int64 = 2016
    let passed = createExitChecksum == expectedCreateExit && parkedChecksum == expectedParked &&
        allocationChecksum > 0 && explicitEpochs == 1500 && foreignAttempts > UInt64(0) &&
        foreignAttached > UInt64(0)
    println("EPOCHHS_STRESS_OK passed=${passed} explicit=${explicitEpochs} last_epoch=${lastEpoch} " +
        "create_exit=${createExitChecksum} parked=${parkedChecksum} allocation=${allocationChecksum} " +
        "foreign_attempts=${foreignAttempts} foreign_attached=${foreignAttached}")
    return if (passed) { 0 } else { 3 }
}
