/** @type {import('jest').Config} */
module.exports = {
  preset: 'jest-expo',
  setupFilesAfterEnv: ['<rootDir>/jest.setup.ts'],
  moduleNameMapper: {
    '^@/(.*)$': '<rootDir>/$1',
    '\\.(png|jpg|jpeg|gif|webp|svg)$': '<rootDir>/test/fileMock.js',
  },
  // __tests__/fixtures holds shared builders, not suites. Without this, jest picks each one
  // up as a test file and fails it for containing no tests.
  testPathIgnorePatterns: ['/node_modules/', '/dist/', '/.expo/', '/__tests__/fixtures/'],
  collectCoverageFrom: [
    'app/**/*.{ts,tsx}',
    'components/**/*.{ts,tsx}',
    'context/**/*.{ts,tsx}',
    'hooks/**/*.{ts,tsx}',
    'services/**/*.{ts,tsx}',
    'constants/**/*.{ts,tsx}',
    '!**/*.d.ts',
  ],
};
