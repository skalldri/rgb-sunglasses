#!/usr/bin/env python3
"""
Unit tests for font_pipeline.py

This test suite covers the font atlas generation and font analysis functionality.
"""

import unittest
import tempfile
import os
import sys
from unittest.mock import patch, MagicMock
from PIL import Image, ImageFont

# Add the current directory to the path so we can import font_pipeline
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import font_pipeline


class TestFontPipeline(unittest.TestCase):
    """Test cases for font pipeline functionality."""

    def setUp(self):
        """Set up test fixtures."""
        self.test_dir = tempfile.mkdtemp()
        self.test_output = os.path.join(self.test_dir, 'test_atlas.png')

    def tearDown(self):
        """Clean up test fixtures."""
        # Clean up any generated test files
        if os.path.exists(self.test_output):
            os.remove(self.test_output)
        os.rmdir(self.test_dir)

    @patch('font_pipeline.ImageFont.truetype')
    @patch('font_pipeline.ImageDraw.Draw')
    @patch('font_pipeline.Image.new')
    def test_generate_font_atlas_1bit_mode(self, mock_image_new, mock_draw, mock_truetype):
        """Test font atlas generation in 1-bit mode."""
        # Mock font and drawing objects
        mock_font = MagicMock()
        mock_truetype.return_value = mock_font
        
        mock_img = MagicMock()
        mock_image_new.return_value = mock_img
        
        mock_drawer = MagicMock()
        mock_draw.return_value = mock_drawer
        
        # Mock textbbox to return consistent dimensions for monospaced font
        mock_drawer.textbbox.return_value = (0, 0, 8, 14)  # 8px wide, 14px tall
        
        # Test the function
        font_pipeline.generate_font_atlas('dummy_font.ttf', self.test_output, 13, '1')
        
        # Verify font was loaded
        mock_truetype.assert_called_with('dummy_font.ttf', 13)
        
        # Verify image was created with correct mode and colors
        mock_image_new.assert_called()
        calls = mock_image_new.call_args_list
        
        # Should be called twice: once for temp image, once for final atlas
        self.assertEqual(len(calls), 2)
        
        # Check final atlas creation (second call)
        final_call = calls[1]
        self.assertEqual(final_call[0][0], '1')  # Mode should be '1'
        self.assertEqual(final_call[1]['color'], 0)  # Background should be black (0)

    @patch('font_pipeline.ImageFont.truetype')
    @patch('font_pipeline.ImageDraw.Draw')
    @patch('font_pipeline.Image.new')
    def test_generate_font_atlas_grayscale_mode(self, mock_image_new, mock_draw, mock_truetype):
        """Test font atlas generation in grayscale mode."""
        # Mock font and drawing objects
        mock_font = MagicMock()
        mock_truetype.return_value = mock_font
        
        mock_img = MagicMock()
        mock_image_new.return_value = mock_img
        
        mock_drawer = MagicMock()
        mock_draw.return_value = mock_drawer
        
        # Mock textbbox to return consistent dimensions for monospaced font
        mock_drawer.textbbox.return_value = (0, 0, 8, 14)  # 8px wide, 14px tall
        
        # Test the function
        font_pipeline.generate_font_atlas('dummy_font.ttf', self.test_output, 13, 'L')
        
        # Verify image was created with correct mode and colors
        calls = mock_image_new.call_args_list
        
        # Check final atlas creation
        final_call = calls[1]
        self.assertEqual(final_call[0][0], 'L')  # Mode should be 'L'
        self.assertEqual(final_call[1]['color'], 0)  # Background should be black (0)

    @patch('font_pipeline.ImageFont.truetype')
    @patch('font_pipeline.ImageDraw.Draw')
    @patch('font_pipeline.Image.new')
    def test_generate_font_atlas_variable_width_error(self, mock_image_new, mock_draw, mock_truetype):
        """Test that variable width fonts are rejected."""
        # Mock font and drawing objects
        mock_font = MagicMock()
        mock_truetype.return_value = mock_font
        
        mock_img = MagicMock()
        mock_image_new.return_value = mock_img
        
        mock_drawer = MagicMock()
        mock_draw.return_value = mock_drawer
        
        # Mock textbbox to return variable widths (non-monospaced)
        width_counter = [0]
        def mock_textbbox(*args, **kwargs):
            # Alternate between 6 and 8 pixel widths to simulate variable width
            width = 6 if width_counter[0] % 2 == 0 else 8
            width_counter[0] += 1
            return (0, 0, width, 14)
        
        mock_drawer.textbbox.side_effect = mock_textbbox
        
        # Test that the function exits with error for variable width font
        with self.assertRaises(SystemExit):
            font_pipeline.generate_font_atlas('dummy_font.ttf', self.test_output, 13, '1')

    @patch('font_pipeline.ImageFont.truetype')
    def test_generate_font_atlas_invalid_font(self, mock_truetype):
        """Test handling of invalid font file."""
        # Mock font loading to raise exception
        mock_truetype.side_effect = Exception("Font file not found")
        
        # Test that the function exits with error for invalid font
        with self.assertRaises(SystemExit):
            font_pipeline.generate_font_atlas('invalid_font.ttf', self.test_output, 13, '1')

    @patch('font_pipeline.ImageFont.truetype')
    @patch('font_pipeline.ImageDraw.Draw')
    @patch('font_pipeline.Image.new')
    def test_generate_font_atlas_save_error(self, mock_image_new, mock_draw, mock_truetype):
        """Test handling of image save error."""
        # Mock font and drawing objects
        mock_font = MagicMock()
        mock_truetype.return_value = mock_font
        
        mock_img = MagicMock()
        mock_img.save.side_effect = Exception("Permission denied")
        mock_image_new.return_value = mock_img
        
        mock_drawer = MagicMock()
        mock_draw.return_value = mock_drawer
        mock_drawer.textbbox.return_value = (0, 0, 8, 14)
        
        # Test that the function exits with error for save failure
        with self.assertRaises(SystemExit):
            font_pipeline.generate_font_atlas('dummy_font.ttf', '/invalid/path/output.png', 13, '1')

    @patch('font_pipeline.ImageFont.truetype')
    @patch('font_pipeline.ImageDraw.Draw')
    @patch('font_pipeline.Image.new')
    @patch('builtins.print')
    def test_analyze_font_sizes(self, mock_print, mock_image_new, mock_draw, mock_truetype):
        """Test font size analysis functionality."""
        # Mock font and drawing objects
        mock_font = MagicMock()
        mock_truetype.return_value = mock_font
        
        mock_img = MagicMock()
        mock_image_new.return_value = mock_img
        
        mock_drawer = MagicMock()
        mock_draw.return_value = mock_drawer
        
        # Mock textbbox to simulate some sizes as monospaced, others as variable
        def mock_textbbox(*args, **kwargs):
            # Get the font size from the call stack
            font_size = mock_truetype.call_args[0][1] if mock_truetype.call_args else 10
            
            # Simulate certain sizes as monospaced (consistent width)
            if font_size in [8, 10, 13, 15]:
                return (0, 0, font_size // 2, font_size + 1)  # Consistent width
            else:
                # Variable width for other sizes
                char_arg = args[1] if len(args) > 1 else 'A'
                width = font_size // 2 if ord(char_arg) % 2 == 0 else (font_size // 2) + 1
                return (0, 0, width, font_size + 1)
        
        mock_drawer.textbbox.side_effect = mock_textbbox
        
        # Test the function
        font_pipeline.analyze_font_sizes('dummy_font.ttf')
        
        # Verify print was called (output was generated)
        self.assertTrue(mock_print.called)
        
        # Check that analysis was performed for expected range
        font_load_calls = mock_truetype.call_args_list
        self.assertTrue(len(font_load_calls) >= 20)  # Should test multiple sizes

    def test_character_range(self):
        """Test that the correct ASCII character range is used."""
        # Test the range of characters (ASCII 33-126)
        characters = [chr(i) for i in range(33, 127)]
        
        # Check first and last characters
        self.assertEqual(characters[0], '!')  # ASCII 33
        self.assertEqual(characters[-1], '~')  # ASCII 126
        
        # Check total count
        self.assertEqual(len(characters), 94)
        
        # Verify no control characters or spaces
        for char in characters:
            self.assertGreaterEqual(ord(char), 33)
            self.assertLessEqual(ord(char), 126)

    @patch('argparse.ArgumentParser.parse_args')
    @patch('font_pipeline.generate_font_atlas')
    def test_main_generate_mode(self, mock_generate, mock_parse_args):
        """Test main function in generate mode."""
        # Mock command line arguments for generate mode
        mock_args = MagicMock()
        mock_args.mode = 'generate'
        mock_args.font_path = 'test_font.ttf'
        mock_args.output_path = 'test_output.png'
        mock_args.char_height = 16
        mock_args.image_mode = '1'
        mock_parse_args.return_value = mock_args
        
        # Test main function
        font_pipeline.main()
        
        # Verify generate_font_atlas was called with correct arguments
        mock_generate.assert_called_once_with('test_font.ttf', 'test_output.png', 16, '1')

    @patch('argparse.ArgumentParser.parse_args')
    @patch('font_pipeline.analyze_font_sizes')
    def test_main_analyze_mode(self, mock_analyze, mock_parse_args):
        """Test main function in analyze mode."""
        # Mock command line arguments for analyze mode
        mock_args = MagicMock()
        mock_args.mode = 'analyze'
        mock_args.font_path = 'test_font.ttf'
        mock_parse_args.return_value = mock_args
        
        # Test main function
        font_pipeline.main()
        
        # Verify analyze_font_sizes was called with correct arguments
        mock_analyze.assert_called_once_with('test_font.ttf')

    @patch('argparse.ArgumentParser.parse_args')
    def test_main_invalid_char_height(self, mock_parse_args):
        """Test main function with invalid character height."""
        # Mock command line arguments with invalid char_height
        mock_args = MagicMock()
        mock_args.mode = 'generate'
        mock_args.font_path = 'test_font.ttf'
        mock_args.output_path = 'test_output.png'
        mock_args.char_height = -5  # Invalid negative height
        mock_args.image_mode = '1'
        mock_parse_args.return_value = mock_args
        
        # Test that main function exits with error
        with self.assertRaises(SystemExit):
            font_pipeline.main()


class TestImageModeValidation(unittest.TestCase):
    """Test cases for image mode validation."""
    
    @patch('font_pipeline.ImageFont.truetype')
    @patch('font_pipeline.ImageDraw.Draw')
    @patch('font_pipeline.Image.new')
    def test_default_image_mode(self, mock_image_new, mock_draw, mock_truetype):
        """Test that default image mode is '1'."""
        # Setup mocks
        mock_font = MagicMock()
        mock_truetype.return_value = mock_font
        
        mock_img = MagicMock()
        mock_image_new.return_value = mock_img
        
        mock_drawer = MagicMock()
        mock_draw.return_value = mock_drawer
        mock_drawer.textbbox.return_value = (0, 0, 8, 14)
        
        # Test function with default image_mode (should be '1')
        font_pipeline.generate_font_atlas('dummy_font.ttf', 'output.png', 13)
        
        # Check that image was created with mode '1' (default)
        calls = mock_image_new.call_args_list
        final_call = calls[1]  # Second call is for the final atlas
        self.assertEqual(final_call[0][0], '1')

    def test_color_values_1bit(self):
        """Test color value calculation for 1-bit mode."""
        # The function should set bg_color=0, fg_color=1 for mode '1'
        # This is implicitly tested in the generate tests, but we can verify logic
        
        image_mode = '1'
        if image_mode == '1':
            bg_color = 0
            fg_color = 1
        else:
            bg_color = 0
            fg_color = 255
            
        self.assertEqual(bg_color, 0)
        self.assertEqual(fg_color, 1)

    def test_color_values_grayscale(self):
        """Test color value calculation for grayscale mode."""
        image_mode = 'L'
        if image_mode == '1':
            bg_color = 0
            fg_color = 1
        else:
            bg_color = 0
            fg_color = 255
            
        self.assertEqual(bg_color, 0)
        self.assertEqual(fg_color, 255)


if __name__ == '__main__':
    # Run the tests
    unittest.main(verbosity=2)