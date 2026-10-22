/* Unit tests for src/lib/usb_node.c.
 *
 * Covers the property constructors and serialization paths that are not
 * reachable through the yaml_loader or yaml_backend tests, specifically:
 *   - create_prop_u32        (missing line 66-74)
 *   - usb_node_clone         (lines 111-112, node-type clone)
 *   - usb_node_validate      errors paths: NULL prop, nested node (lines 167-170,
 *                             202-203, 211-212, 216, 218-220)
 *   - usb_node_serialize     UINT32 and nested NODE paths (lines 167-170)
 */

#include <stdlib.h>
#include <stdint.h>
#include <check.h>
#include <glib.h>

#include "usb_node.h"

/* ── create_prop_u32 ── */

START_TEST(test_create_prop_u32_values)
{
    usb_prop_t *p = create_prop_u32("baud", 0xDEADBEEF);
    ck_assert_ptr_nonnull(p);
    ck_assert_str_eq(p->name, "baud");
    ck_assert_int_eq(p->type, USB_PROP_UINT32);
    ck_assert_uint_eq(p->val.u32, 0xDEADBEEF);
    ck_assert_uint_eq(p->size, 4);

    usb_node_t *node = usb_node_create();
    usb_node_set_prop(node, p);
    usb_node_free(node);
}
END_TEST

/* ── usb_node_clone with nested USB_PROP_NODE ── */

START_TEST(test_clone_nested_node)
{
    usb_node_t *child = usb_node_create();
    usb_node_set_prop(child, create_prop_u8("bLength", 9));
    usb_node_set_prop(child, create_prop_u16("wValue", 0x1234));

    usb_node_t *parent = usb_node_create();
    usb_node_set_prop(parent, create_prop_node("interface_descriptor", child));

    usb_node_t *clone = usb_node_clone(parent);
    ck_assert_ptr_nonnull(clone);

    /* The clone must have one property of NODE type. */
    GList *l = clone->props;
    ck_assert_ptr_nonnull(l);
    usb_prop_t *p = l->data;
    ck_assert_str_eq(p->name, "interface_descriptor");
    ck_assert_int_eq(p->type, USB_PROP_NODE);
    ck_assert_ptr_nonnull(p->val.node);

    /* The nested node must carry the two child props independently. */
    usb_node_t *cloned_child = p->val.node;
    ck_assert_ptr_nonnull(cloned_child->props);
    ck_assert_ptr_nonnull(cloned_child->props->next);

    usb_node_free(parent);
    usb_node_free(clone);
}
END_TEST

START_TEST(test_clone_null)
{
    ck_assert_ptr_null(usb_node_clone(NULL));
}
END_TEST

/* ── usb_node_serialize with UINT32 and nested NODE ── */

START_TEST(test_serialize_u32)
{
    usb_node_t *node = usb_node_create();
    usb_node_set_prop(node, create_prop_u32("field", 0x01020304));

    GByteArray *buf = g_byte_array_new();
    usb_node_serialize(node, buf);

    ck_assert_uint_eq(buf->len, 4);
    /* Little-endian: bytes are 04 03 02 01 */
    ck_assert_uint_eq(buf->data[0], 0x04);
    ck_assert_uint_eq(buf->data[1], 0x03);
    ck_assert_uint_eq(buf->data[2], 0x02);
    ck_assert_uint_eq(buf->data[3], 0x01);

    g_byte_array_unref(buf);
    usb_node_free(node);
}
END_TEST

START_TEST(test_serialize_nested_node)
{
    usb_node_t *child = usb_node_create();
    usb_node_set_prop(child, create_prop_u8("byte", 0xAB));

    usb_node_t *parent = usb_node_create();
    usb_node_set_prop(parent, create_prop_u8("before", 0x01));
    usb_node_set_prop(parent, create_prop_node("sub", child));
    usb_node_set_prop(parent, create_prop_u8("after", 0x02));

    GByteArray *buf = g_byte_array_new();
    usb_node_serialize(parent, buf);

    /* Should be: 0x01, 0xAB, 0x02 */
    ck_assert_uint_eq(buf->len, 3);
    ck_assert_uint_eq(buf->data[0], 0x01);
    ck_assert_uint_eq(buf->data[1], 0xAB);
    ck_assert_uint_eq(buf->data[2], 0x02);

    g_byte_array_unref(buf);
    usb_node_free(parent);
}
END_TEST

/* ── usb_node_validate ── */

START_TEST(test_validate_all_typed_ok)
{
    usb_node_t *node = usb_node_create();
    usb_node_set_prop(node, create_prop_u8("a", 1));
    usb_node_set_prop(node, create_prop_u16("b", 2));
    usb_node_set_prop(node, create_prop_u32("c", 3));

    uint8_t arr[2] = { 0xAA, 0xBB };
    usb_node_set_prop(node, create_prop_array("d", arr, sizeof(arr)));

    usb_node_t *child = usb_node_create();
    usb_node_set_prop(child, create_prop_u8("x", 0));
    usb_node_set_prop(node, create_prop_node("sub", child));

    ck_assert_int_eq(usb_node_validate(node, 0), 0);
    usb_node_free(node);
}
END_TEST

START_TEST(test_validate_null_prop_returns_minus_one)
{
    usb_node_t *node = usb_node_create();
    usb_node_set_prop(node, create_prop_u8("good", 42));
    usb_node_set_prop(node, create_prop_null("bad"));

    ck_assert_int_eq(usb_node_validate(node, 0), -1);
    usb_node_free(node);
}
END_TEST

START_TEST(test_validate_nested_null_prop_returns_minus_one)
{
    usb_node_t *child = usb_node_create();
    usb_node_set_prop(child, create_prop_null("unset"));

    usb_node_t *parent = usb_node_create();
    usb_node_set_prop(parent, create_prop_node("child", child));

    ck_assert_int_eq(usb_node_validate(parent, 0), -1);
    usb_node_free(parent);
}
END_TEST

/* ── usb_node_set_prop replace in-place ── */

START_TEST(test_set_prop_replace)
{
    usb_node_t *node = usb_node_create();
    usb_node_set_prop(node, create_prop_u8("field", 0x10));
    usb_node_set_prop(node, create_prop_u8("field", 0x20)); /* replace */

    GByteArray *buf = g_byte_array_new();
    usb_node_serialize(node, buf);
    ck_assert_uint_eq(buf->len, 1);
    ck_assert_uint_eq(buf->data[0], 0x20);

    g_byte_array_unref(buf);
    usb_node_free(node);
}
END_TEST

/* ── usb_node_merge ── */

START_TEST(test_merge_copies_props)
{
    usb_node_t *src = usb_node_create();
    usb_node_set_prop(src, create_prop_u8("x", 0xAA));
    usb_node_set_prop(src, create_prop_u16("y", 0xBBCC));

    usb_node_t *dst = usb_node_create();
    usb_node_merge(dst, src);

    GByteArray *buf = g_byte_array_new();
    usb_node_serialize(dst, buf);
    /* 1 byte for u8 + 2 bytes for u16 */
    ck_assert_uint_eq(buf->len, 3);

    g_byte_array_unref(buf);
    usb_node_free(src);
    usb_node_free(dst);
}
END_TEST

START_TEST(test_merge_null_guards)
{
    usb_node_t *node = usb_node_create();
    /* Neither of these should crash. */
    usb_node_merge(node, NULL);
    usb_node_merge(NULL, node);
    usb_node_free(node);
}
END_TEST

/* ── usb_node_canonicalize_order ── */

START_TEST(test_canonicalize_reorders_scrambled_device_descriptor)
{
    usb_node_t *node = usb_node_create();

    /* Written in a scrambled order, as a user might type it by hand. */
    usb_node_set_prop(node, create_prop_u16("idProduct", 0x1702));
    usb_node_set_prop(node, create_prop_u16("idVendor", 0x04d9));
    usb_node_set_prop(node, create_prop_u8("bLength", 18));
    usb_node_set_prop(node, create_prop_u8("bDescriptorType", 0x01));
    usb_node_set_prop(node, create_prop_u16("bcdUSB", 0x0200));
    usb_node_set_prop(node, create_prop_u8("bDeviceClass", 0));
    usb_node_set_prop(node, create_prop_u8("bDeviceSubClass", 0));
    usb_node_set_prop(node, create_prop_u8("bDeviceProtocol", 0));
    usb_node_set_prop(node, create_prop_u8("bMaxPacketSize0", 8));
    usb_node_set_prop(node, create_prop_u16("bcdDevice", 0x0406));
    usb_node_set_prop(node, create_prop_u8("iManufacturer", 1));
    usb_node_set_prop(node, create_prop_u8("iProduct", 2));
    usb_node_set_prop(node, create_prop_u8("iSerialNumber", 0));
    usb_node_set_prop(node, create_prop_u8("bNumConfigurations", 1));

    usb_node_canonicalize_order(node, "device_descriptor");

    GByteArray *buf = g_byte_array_new();
    usb_node_serialize(node, buf);

    /* USB 2.0 table 9-8 byte layout, regardless of the scrambled input
     * order above. */
    const uint8_t expected[] = {
        18, 0x01,           /* bLength, bDescriptorType */
        0x00, 0x02,         /* bcdUSB = 0x0200 LE */
        0x00, 0x00, 0x00,   /* bDeviceClass/SubClass/Protocol */
        8,                  /* bMaxPacketSize0 */
        0xd9, 0x04,         /* idVendor = 0x04d9 LE */
        0x02, 0x17,         /* idProduct = 0x1702 LE */
        0x06, 0x04,         /* bcdDevice = 0x0406 LE */
        1, 2, 0,            /* iManufacturer, iProduct, iSerialNumber */
        1,                  /* bNumConfigurations */
    };
    ck_assert_uint_eq(buf->len, sizeof(expected));
    ck_assert_mem_eq(buf->data, expected, sizeof(expected));

    g_byte_array_unref(buf);
    usb_node_free(node);
}
END_TEST

START_TEST(test_canonicalize_unknown_kind_is_noop)
{
    usb_node_t *node = usb_node_create();
    usb_node_set_prop(node, create_prop_u8("zebra", 1));
    usb_node_set_prop(node, create_prop_u8("alpha", 2));

    usb_node_canonicalize_order(node, "totally_custom_block");

    GList *l = node->props;
    ck_assert_ptr_nonnull(l);
    ck_assert_str_eq(((usb_prop_t *)l->data)->name, "zebra");
    l = l->next;
    ck_assert_ptr_nonnull(l);
    ck_assert_str_eq(((usb_prop_t *)l->data)->name, "alpha");

    usb_node_free(node);
}
END_TEST

START_TEST(test_canonicalize_appends_noncanonical_after_header)
{
    usb_node_t *node = usb_node_create();

    /* hid_descriptor is nested in the middle, as the HID example YAML
     * writes it (interleaved with the interface header fields). It is not
     * part of the interface descriptor's fixed header. */
    usb_node_set_prop(node, create_prop_u8("bLength", 9));
    usb_node_set_prop(node, create_prop_u8("bDescriptorType", 0x04));
    usb_node_set_prop(node, create_prop_node("hid_descriptor", usb_node_create()));
    usb_node_set_prop(node, create_prop_u8("bInterfaceNumber", 0));
    usb_node_set_prop(node, create_prop_u8("bAlternateSetting", 0));
    usb_node_set_prop(node, create_prop_u8("bNumEndpoints", 1));
    usb_node_set_prop(node, create_prop_u8("bInterfaceClass", 3));
    usb_node_set_prop(node, create_prop_u8("bInterfaceSubClass", 1));
    usb_node_set_prop(node, create_prop_u8("bInterfaceProtocol", 1));
    usb_node_set_prop(node, create_prop_u8("iInterface", 0));

    usb_node_canonicalize_order(node, "interfaces");

    /* The 9 canonical header fields come first, in spec order; the nested
     * hid_descriptor is pushed after them. */
    const char *expected_order[] = {
        "bLength", "bDescriptorType", "bInterfaceNumber", "bAlternateSetting",
        "bNumEndpoints", "bInterfaceClass", "bInterfaceSubClass",
        "bInterfaceProtocol", "iInterface", "hid_descriptor",
    };
    GList *l = node->props;
    for (size_t i = 0; i < G_N_ELEMENTS(expected_order); i++, l = l->next) {
        ck_assert_ptr_nonnull(l);
        ck_assert_str_eq(((usb_prop_t *)l->data)->name, expected_order[i]);
    }
    ck_assert_ptr_null(l);

    usb_node_free(node);
}
END_TEST

START_TEST(test_canonicalize_null_guards)
{
    /* Neither of these should crash. */
    usb_node_canonicalize_order(NULL, "device_descriptor");

    usb_node_t *node = usb_node_create();
    usb_node_set_prop(node, create_prop_u8("bLength", 9));
    usb_node_canonicalize_order(node, NULL);
    usb_node_free(node);
}
END_TEST

static Suite *usb_node_suite(void)
{
    Suite *s = suite_create("usb_node");

    TCase *tc_prop = tcase_create("prop_constructors");
    tcase_add_test(tc_prop, test_create_prop_u32_values);
    suite_add_tcase(s, tc_prop);

    TCase *tc_clone = tcase_create("clone");
    tcase_add_test(tc_clone, test_clone_nested_node);
    tcase_add_test(tc_clone, test_clone_null);
    suite_add_tcase(s, tc_clone);

    TCase *tc_ser = tcase_create("serialize");
    tcase_add_test(tc_ser, test_serialize_u32);
    tcase_add_test(tc_ser, test_serialize_nested_node);
    suite_add_tcase(s, tc_ser);

    TCase *tc_val = tcase_create("validate");
    tcase_add_test(tc_val, test_validate_all_typed_ok);
    tcase_add_test(tc_val, test_validate_null_prop_returns_minus_one);
    tcase_add_test(tc_val, test_validate_nested_null_prop_returns_minus_one);
    suite_add_tcase(s, tc_val);

    TCase *tc_mut = tcase_create("mutation");
    tcase_add_test(tc_mut, test_set_prop_replace);
    tcase_add_test(tc_mut, test_merge_copies_props);
    tcase_add_test(tc_mut, test_merge_null_guards);
    suite_add_tcase(s, tc_mut);

    TCase *tc_canon = tcase_create("canonicalize_order");
    tcase_add_test(tc_canon, test_canonicalize_reorders_scrambled_device_descriptor);
    tcase_add_test(tc_canon, test_canonicalize_unknown_kind_is_noop);
    tcase_add_test(tc_canon, test_canonicalize_appends_noncanonical_after_header);
    tcase_add_test(tc_canon, test_canonicalize_null_guards);
    suite_add_tcase(s, tc_canon);

    return s;
}

int main(void)
{
    SRunner *sr = srunner_create(usb_node_suite());
    srunner_run_all(sr, CK_NORMAL);
    int failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
