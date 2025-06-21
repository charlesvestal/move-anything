export function clamp(value, min, max) {
    if (value > max) {
        return max;
    }
    
    if (value < min) {
        return min;
    }
    
    return value;
}