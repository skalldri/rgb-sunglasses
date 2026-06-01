import { ThemedText } from '@/components/themed-text';
import { ThemedView } from '@/components/themed-view';
import { useBluetooth } from '@/context/bluetooth-context';
import { encodeColorToBase64 } from '@/services/ble-value-codec';
import Slider from '@react-native-community/slider';
import { useLocalSearchParams, useRouter } from 'expo-router';
import { useRef, useState } from 'react';
import { GestureResponderEvent, Pressable, StyleSheet, View } from 'react-native';

// Convert RGB to HSV
function rgbToHsv(r: number, g: number, b: number): [number, number, number] {
    r /= 255;
    g /= 255;
    b /= 255;

    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    const delta = max - min;

    let h = 0;
    if (delta !== 0) {
        if (max === r) {
            h = 60 * (((g - b) / delta) % 6);
        } else if (max === g) {
            h = 60 * ((b - r) / delta + 2);
        } else {
            h = 60 * ((r - g) / delta + 4);
        }
    }
    if (h < 0) h += 360;

    const s = max === 0 ? 0 : delta / max;
    const v = max;

    return [h, s, v];
}

// Convert HSV to RGB
function hsvToRgb(h: number, s: number, v: number): [number, number, number] {
    const c = v * s;
    const x = c * (1 - Math.abs((h / 60) % 2 - 1));
    const m = v - c;

    let r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }

    return [
        Math.round((r + m) * 255),
        Math.round((g + m) * 255),
        Math.round((b + m) * 255),
    ];
}

const WHEEL_SIZE = 250;
const WHEEL_RADIUS = WHEEL_SIZE / 2;

export default function ColorPickerModal() {
    const params = useLocalSearchParams();
    const router = useRouter();
    const { writeToCharacteristic } = useBluetooth();

    // Get the characteristic UUID from params
    const charUuid = params.charUuid as string;

    // Parse RGB values from query parameters
    const initialR = params.r ? parseInt(params.r as string, 10) : 255;
    const initialG = params.g ? parseInt(params.g as string, 10) : 0;
    const initialB = params.b ? parseInt(params.b as string, 10) : 0;

    // Convert initial RGB to HSV
    const [initialHue, initialSaturation, initialBrightness] = rgbToHsv(initialR, initialG, initialB);

    const [hue, setHue] = useState(initialHue);
    const [saturation, setSaturation] = useState(initialSaturation);
    const brightness = 1; // Fixed at full brightness

    const [rgb, setRgb] = useState<[number, number, number]>([initialR, initialG, initialB]);
    const wheelRef = useRef<View>(null);
    const wheelLayout = useRef<{ x: number; y: number } | null>(null);

    const colorHex = `#${rgb[0].toString(16).padStart(2, '0')}${rgb[1].toString(16).padStart(2, '0')}${rgb[2].toString(16).padStart(2, '0')}`;

    function updateColor(newHue: number, newSaturation: number) {
        setHue(newHue);
        setSaturation(newSaturation);
        setRgb(hsvToRgb(newHue, newSaturation, brightness));
    }

    function handleWheelTouch(event: GestureResponderEvent) {
        const { pageX, pageY } = event.nativeEvent;

        // Use measured layout position
        if (!wheelLayout.current) return;

        const x = pageX - wheelLayout.current.x - WHEEL_RADIUS;
        const y = pageY - wheelLayout.current.y - WHEEL_RADIUS;

        // Calculate angle (hue) and distance (saturation)
        // atan2 gives 0° at right, but wheel has 0° at top, so add 90°
        let angle = Math.atan2(y, x) * (180 / Math.PI) + 90;
        if (angle < 0) angle += 360;
        if (angle >= 360) angle -= 360;

        const distance = Math.sqrt(x * x + y * y);
        const normalizedDistance = Math.min(distance / WHEEL_RADIUS, 1);

        updateColor(angle, normalizedDistance);
    }

    function measureWheel() {
        wheelRef.current?.measureInWindow((x, y) => {
            wheelLayout.current = { x, y };
        });
    }

    // Generate hue wheel colors for the conic gradient effect using segments
    const wheelSegments = [];
    for (let i = 0; i < 360; i += 10) {
        const [r, g, b] = hsvToRgb(i, 1, 1);
        wheelSegments.push(
            <View
                key={i}
                style={{
                    position: 'absolute',
                    width: WHEEL_SIZE,
                    height: WHEEL_SIZE,
                    borderRadius: WHEEL_RADIUS,
                    backgroundColor: `rgb(${r},${g},${b})`,
                    opacity: 0.1,
                    transform: [{ rotate: `${i}deg` }],
                }}
            />
        );
    }

    // Calculate thumb position on wheel
    // Subtract 90° to convert from hue (0° at top) to math angle (0° at right)
    const thumbAngle = ((hue - 90) * Math.PI) / 180;
    const thumbDistance = saturation * (WHEEL_RADIUS - 15);
    const thumbX = WHEEL_RADIUS + Math.cos(thumbAngle) * thumbDistance - 15;
    const thumbY = WHEEL_RADIUS + Math.sin(thumbAngle) * thumbDistance - 15;

    return (
        <ThemedView style={styles.container}>
            <ThemedText type="title">Color Picker</ThemedText>

            {/* Hue Wheel */}
            <View
                ref={wheelRef}
                style={styles.wheelContainer}
                onLayout={measureWheel}
                onStartShouldSetResponder={() => true}
                onMoveShouldSetResponder={() => true}
                onResponderGrant={(e) => { measureWheel(); handleWheelTouch(e); }}
                onResponderMove={handleWheelTouch}
            >
                {/* Color wheel background using SVG-like approach */}
                <View style={styles.wheel}>
                    {/* Create color wheel with radial segments */}
                    {Array.from({ length: 360 }, (_, i) => {
                        const [r, g, b] = hsvToRgb(i, 1, 1);
                        return (
                            <View
                                key={i}
                                style={{
                                    position: 'absolute',
                                    width: 2,
                                    height: WHEEL_RADIUS,
                                    backgroundColor: `rgb(${r},${g},${b})`,
                                    left: WHEEL_RADIUS - 1,
                                    top: 0,
                                    transformOrigin: `1px ${WHEEL_RADIUS}px`,
                                    transform: [{ rotate: `${i}deg` }],
                                }}
                            />
                        );
                    })}
                    {/* White center overlay for saturation gradient effect */}
                    <View style={styles.wheelCenterGradient} />
                </View>

                {/* Thumb indicator */}
                <View style={[styles.wheelThumb, { left: thumbX, top: thumbY, backgroundColor: colorHex }]} />
            </View>

            <View style={[styles.colorPreview, { backgroundColor: colorHex }]} />

            <ThemedText style={styles.colorHex}>{colorHex.toUpperCase()}</ThemedText>

            <View style={styles.sliderContainer}>
                <ThemedText style={styles.sliderLabel}>Saturation: {Math.round(saturation * 100)}%</ThemedText>
                <Slider
                    style={styles.slider}
                    minimumValue={0}
                    maximumValue={1}
                    step={0.01}
                    value={saturation}
                    onValueChange={(value) => updateColor(hue, value)}
                    minimumTrackTintColor={colorHex}
                    maximumTrackTintColor="#666"
                    thumbTintColor={colorHex}
                />
            </View>

            <Pressable
                style={styles.link}
                onPress={async () => {
                    if (charUuid) {
                        const encoded = encodeColorToBase64({ r: rgb[0], g: rgb[1], b: rgb[2] });
                        await writeToCharacteristic(charUuid, encoded);
                    }
                    router.back();
                }}
            >
                <ThemedText type="link">Done</ThemedText>
            </Pressable>
        </ThemedView>
    );
}

const styles = StyleSheet.create({
    container: {
        flex: 1,
        alignItems: 'center',
        justifyContent: 'center',
        padding: 20,
    },
    wheelContainer: {
        width: WHEEL_SIZE,
        height: WHEEL_SIZE,
        marginVertical: 20,
    },
    wheel: {
        width: WHEEL_SIZE,
        height: WHEEL_SIZE,
        borderRadius: WHEEL_RADIUS,
        overflow: 'hidden',
    },
    wheelCenterGradient: {
        position: 'absolute',
        width: WHEEL_SIZE * 0.7,
        height: WHEEL_SIZE * 0.7,
        borderRadius: WHEEL_SIZE * 0.35,
        left: WHEEL_SIZE * 0.15,
        top: WHEEL_SIZE * 0.15,
        backgroundColor: 'rgba(255,255,255,0.3)',
    },
    wheelThumb: {
        position: 'absolute',
        width: 30,
        height: 30,
        borderRadius: 15,
        borderWidth: 3,
        borderColor: '#fff',
        shadowColor: '#000',
        shadowOffset: { width: 0, height: 2 },
        shadowOpacity: 0.5,
        shadowRadius: 3,
        elevation: 5,
    },
    colorPreview: {
        width: 80,
        height: 80,
        borderRadius: 40,
        marginVertical: 10,
        borderWidth: 2,
        borderColor: '#fff',
    },
    colorHex: {
        fontSize: 24,
        fontWeight: 'bold',
        marginBottom: 10,
    },
    sliderContainer: {
        width: '100%',
        marginVertical: 10,
    },
    sliderLabel: {
        marginBottom: 5,
    },
    slider: {
        width: '100%',
        height: 40,
    },
    link: {
        marginTop: 20,
        paddingVertical: 15,
    },
});
