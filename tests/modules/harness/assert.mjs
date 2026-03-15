/*
 * Minimal assertion helpers for Move module tests.
 */

let passed = 0;
let failed = 0;
const failures = [];

function pass(label) {
    passed++;
    process.stdout.write('.');
}

function fail(label, message) {
    failed++;
    failures.push(`  FAIL: ${label}\n        ${message}`);
    process.stdout.write('F');
}

export function assertEqual(actual, expected, label) {
    if (actual === expected) {
        pass(label);
    } else {
        fail(label, `expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
    }
}

export function assertNotEqual(actual, unexpected, label) {
    if (actual !== unexpected) {
        pass(label);
    } else {
        fail(label, `expected value to differ from ${JSON.stringify(unexpected)}`);
    }
}

export function assertArrayEqual(actual, expected, label) {
    const a = JSON.stringify(actual);
    const e = JSON.stringify(expected);
    if (a === e) {
        pass(label);
    } else {
        fail(label, `expected ${e}, got ${a}`);
    }
}

export function assertTrue(value, label) {
    if (value) {
        pass(label);
    } else {
        fail(label, `expected truthy, got ${JSON.stringify(value)}`);
    }
}

export function assertFalse(value, label) {
    if (!value) {
        pass(label);
    } else {
        fail(label, `expected falsy, got ${JSON.stringify(value)}`);
    }
}

export function assertThrows(fn, label) {
    try {
        fn();
        fail(label, 'expected function to throw, but it did not');
    } catch (e) {
        pass(label);
    }
}

export function summarize(suiteName = '') {
    const total = passed + failed;
    console.log(`\n${suiteName ? suiteName + ': ' : ''}${passed}/${total} passed`);
    if (failures.length) {
        console.log('\nFailures:');
        for (const f of failures) console.log(f);
    }
    if (failed > 0) process.exit(1);
}

export function resetCounts() {
    passed = 0;
    failed = 0;
    failures.length = 0;
}
